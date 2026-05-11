# Actor Lifecycle State Machine Design

**Issue:** [#6 [ACT-001]](https://github.com/skg7on/HPActor/issues/6)
**Date:** 2026-05-11
**Status:** Design approved, pending implementation

## 1. Summary

Define a formal actor lifecycle state machine that provides a shared contract used by supervision, shutdown, CLI, metrics, and routing. Today there are actor states in scheduling (`ActorState`) and lifecycle hooks (`on_activate`/`on_deactivate`), but no unified lifecycle model. This design adds an opt-in `LifecycleActor` mixin with 7 core states and declarative transition rules.

## 2. States

| State | Enum | Semantics |
|-------|------|-----------|
| STARTING | `kStarting` | Spawn called, wiring in progress. User messages queued, system messages processed. |
| ACTIVE | `kActive` | Normal operation. All messages accepted and dispatched. |
| DRAINING | `kDraining` | Finishing queued work. New user messages rejected, system messages accepted. |
| STOPPING | `kStopping` | Cleanup in progress. User messages rejected, system messages accepted. |
| STOPPED | `kStopped` | Terminal. No messages accepted. Can transition to STARTING via restart (new incarnation). |
| FAILED | `kFailed` | Transient terminal. Actor crashed. Supervision decides: restart, stop, or recover. |
| RECOVERING | `kRecovering` | Rehydrating after failure. User messages queued, system messages processed. |

HIBERNATING and PASSIVATED are deferred to a future iteration.

## 3. State Machine

### 3.1 Declarative Definition

Each state owns its transition rules via a `constexpr StateDef` array:

```cpp
enum class LifecycleState : uint8_t {
    kStarting = 0, kActive = 1, kDraining = 2, kStopping = 3,
    kStopped = 4, kFailed = 5, kRecovering = 6,
};

struct StateDef {
    LifecycleState state;
    const char*    name;
    bool           accepts_user_msgs   : 1;
    bool           accepts_system_msgs : 1;
    uint8_t        num_transitions     : 3;
    LifecycleState transitions[7];
};

constexpr StateDef kStateMachine[] = {
    { kStarting,   "starting",   {0, 1}, 1, {kActive, kFailed}},
    { kActive,     "active",     {1, 1}, 3, {kDraining, kStopping, kFailed}},
    { kDraining,   "draining",   {0, 1}, 2, {kStopping, kFailed}},
    { kStopping,   "stopping",   {0, 1}, 2, {kStopped, kFailed}},
    { kStopped,    "stopped",    {0, 0}, 1, {kStarting}},
    { kFailed,     "failed",     {0, 1}, 3, {kStarting, kStopped, kRecovering}},
    { kRecovering, "recovering", {0, 1}, 2, {kActive, kFailed}},
};
```

### 3.2 Transition Rules

- State is stored as `std::atomic<uint8_t>` in `LifecycleActor`.
- `transition(LifecycleState to)` validates that `to` is in the current state's transition list, then CAS from current → target.
- If the CAS succeeds, the corresponding virtual hook is invoked.
- If the CAS fails (state changed concurrently), returns `false`.
- Hooks fire **after** the state change (post-transition pattern).

### 3.3 Hook Dispatch

| Transition | Hook |
|------------|------|
| STARTING → ACTIVE | `on_start()` |
| RECOVERING → ACTIVE | `on_start()` (reuse) |
| ACTIVE → DRAINING | `on_drain()` |
| * → STOPPING | `on_stop()` |
| STOPPING → STOPPED | `on_deactivate()` |
| * → FAILED | `on_fail(error)` |
| FAILED/STOPPED → STARTING | `on_restart()` |
| FAILED → RECOVERING | `on_recover()` |

## 4. Message Gating

- **User messages** (TypeTag >= 100): accepted only in `ACTIVE`.
  - In STARTING/RECOVERING: queued for delivery after activation.
  - In DRAINING/STOPPING/FAILED: rejected with dead-letter reason `kNotAcceptingMessages`.
  - In STOPPED: rejected.
- **System messages** (TypeTag < 100): accepted in all states except `STOPPED`.
  - Link/Unlink/Monitor/Demonitor/Down, SpawnReq/Resp, SystemInit, CLI commands, Metrics requests always pass through.

The gate is a single check at the top of `EventBasedActor::receive()`, after system message interception but before user message dispatch.

## 5. LifecycleActor Mixin

Opt-in virtual mixin class. Actors that need lifecycle management inherit both their actor base and `LifecycleActor`.

```cpp
class LifecycleActor {
public:
    LifecycleActor();
    virtual ~LifecycleActor() = default;

    // State queries
    LifecycleState state() const noexcept;
    bool accepts_user_msgs() const noexcept;
    bool accepts_system_msgs() const noexcept;
    const char* state_string() const noexcept;
    uint64_t incarnation() const noexcept;
    void bump_incarnation();

    // Transition: validates, CAS, invokes hook
    bool transition(LifecycleState to);

    // Virtual hooks (default = no-op)
    virtual void on_start()    {}
    virtual void on_drain()    {}
    virtual void on_stop()     {}
    virtual void on_deactivate() {}
    virtual void on_fail(error reason) {}
    virtual void on_recover()  {}
    virtual void on_restart()  {}

protected:
    std::atomic<uint8_t> state_;
    std::atomic<uint64_t> incarnation_{0};
};
```

## 6. Actor Hierarchy Integration

`AbstractActor` gains a virtual `as_lifecycle()` method returning `nullptr` by default. `LifecycleActor` overrides it to return `this`. This provides RTTI-free downcasting.

Usage:
```cpp
class MyWorker : public EventBasedActor, public LifecycleActor {
    void on_start() override { /* wire up */ }
    // ... message handlers ...
};
```

## 7. Subsystem Integration

### 7.1 ActorSystem::spawn()
After wiring (context, mailbox, scheduler) and `on_activate()`:
1. Check `as_lifecycle()`, if non-null transition STARTING → ACTIVE.
2. Emit `kActorSpawned` metric as before.

### 7.2 Supervision
`SupervisorActor::restart_child()`:
1. Transition child: current → FAILED (calls `on_fail`).
2. Check restart policy. If exceeded: transition FAILED → STOPPED, erase child.
3. Otherwise: bump incarnation, transition FAILED → STARTING (calls `on_restart`).
4. Respawn child; child's `on_start()` transitions → ACTIVE.

### 7.3 CLI
- `to_metadata()` reads lifecycle state via `as_lifecycle()`. Falls back to `"unknown"` for non-lifecycle actors.
- `KillRequest` handler: transition to STOPPING → STOPPED.
- New `/actor <id> drain` command: transition ACTIVE → DRAINING.

### 7.4 Metrics
- New `kLifecycleTransition` event emitted from `LifecycleActor::transition()`.
- New `kMessageRejected` event emitted when user message is gated.
- Aggregator produces per-state counters and transition counters.

## 8. Files

### New
| File | Purpose |
|------|---------|
| `include/hpactor/actor/lifecycle_state.hpp` | LifecycleState enum, constexpr StateDef table |
| `include/hpactor/actor/lifecycle_actor.hpp` | LifecycleActor mixin class |
| `src/actor/lifecycle_actor.cpp` | transition() impl, hook dispatch |
| `tests/actor/test_lifecycle_state.cpp` | 12 state machine unit tests |
| `tests/actor/test_lifecycle_actor.cpp` | 8 integration tests |

### Modified
| File | Change |
|------|--------|
| `include/hpactor/actor/abstract_actor.hpp` | Add `virtual LifecycleActor* as_lifecycle()` |
| `src/actor/abstract_actor.cpp` | Update `to_metadata()` for lifecycle state |
| `src/actor/event_based_actor.cpp` | Lifecycle gate in `receive()`, Kill handler update |
| `src/actor/actor_system.cpp` | Wire lifecycle in `spawn()` |
| `src/supervision/supervision.cpp` | Drive FAILED→STARTING→ACTIVE in restart |
| `include/hpactor/metrics/metrics_event.hpp` | Add kLifecycleTransition, kMessageRejected |
| `src/metrics/metrics_aggregator.cpp` | Lifecycle transition aggregation |
| `tests/CMakeLists.txt` | Add new test targets |

## 9. Test Plan

### test_lifecycle_state (12 tests)
1. Default state is STARTING
2. Legal transition STARTING→ACTIVE succeeds
3. Illegal transition ACTIVE→STARTING fails
4. Illegal transition ACTIVE→RECOVERING fails
5. Full happy path: STARTING→ACTIVE→DRAINING→STOPPING→STOPPED
6. Failure path: ACTIVE→FAILED→STARTING→ACTIVE
7. Recovery path: FAILED→RECOVERING→ACTIVE
8. state_string() returns correct name for each state
9. accepts_user_msgs() true only in ACTIVE
10. accepts_system_msgs() false only in STOPPED
11. transition invokes correct hook
12. Incarnation bumps on restart path

### test_lifecycle_actor (8 integration tests)
1. Actor without LifecycleActor returns as_lifecycle()=nullptr
2. Lifecycle actor spawns and transitions to ACTIVE
3. User messages rejected when not ACTIVE
4. System messages accepted in all non-STOPPED states
5. to_metadata() reports correct lifecycle state
6. Default actor (no lifecycle) to_metadata() says "unknown"
7. KillRequest drives STOPPING→STOPPED
8. Supervisor restarts failed lifecycle child

## 10. Backward Compatibility

- Actors without `LifecycleActor` mixin are completely unaffected.
- `as_lifecycle()` returns `nullptr` by default — all lifecycle paths are no-ops.
- `to_metadata()` falls back to `"unknown"` for non-lifecycle actors.
- Existing 126 tests continue to pass without modification.
- No RTTI, no exceptions, no new dependencies.
