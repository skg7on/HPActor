# Akka Gap Closure Sprint 1 — Design Spec

Closes 5 gaps from issue #329 (Akka Typed Actors Gap Analysis):
Actor Receptionist, BehaviorTestKit, TestProbe, MessageAdapter, and
CoordinatedShutdown user-defined phases.

## Gap Inventory

The original issue #329 identified 24 missing and 14 partial gaps. Five were
closed in PRs #328–#334 (fluent API, behavior combinators, FSM DSL, StashBuffer,
routers). This sprint closes 5 more:

| # | Gap | Issue #329 Status | Tier |
|---|-----|-------------------|------|
| 1 | Actor Receptionist | Missing | P0 |
| 2 | BehaviorTestKit | Missing | P0 |
| 3 | TestProbe | Missing | P0 |
| 4 | MessageAdapter | Missing | P0 |
| 5 | CoordinatedShutdown user phases | Partial | P0 |

## Architecture Overview

```
include/hpactor/
├── actor/
│   ├── receptionist/                    # NEW
│   │   ├── service_key.hpp              #   ServiceKey<T> template
│   │   ├── receptionist_messages.hpp    #   Internal message types
│   │   └── receptionist.hpp             #   Receptionist system actor
│   ├── testing/                         # NEW (header-only test support)
│   │   ├── behavior_test_kit.hpp        #   Synchronous Behavior testing
│   │   └── test_probe.hpp               #   Async message assertion helper
│   ├── routing/                         # EXTENSION
│   │   └── group_router.hpp             #   ServiceKey constructor
│   ├── behavior.hpp                     # EXTENSION — message_adapter combinator
│   └── lifecycle/
│       └── shutdown_coordinator.hpp     # EXTENSION — user-defined phases
src/actor/
├── receptionist/
│   └── receptionist.cpp                 # NEW
├── routing/
│   └── group_router.cpp                 # EXTENSION
└── lifecycle/
    └── shutdown_coordinator.cpp         # EXTENSION
tests/unit/actor/
├── receptionist/
│   └── test_receptionist.cpp            # NEW
├── lifecycle/
│   └── test_shutdown_coordinator.cpp    # EXTENSION
├── routing/
│   └── test_group_router.cpp            # EXTENSION
├── test_behavior_message_adapter.cpp    # NEW
├── behavior_test_kit_test.cpp           # NEW (dogfood)
└── test_probe_test.cpp                  # NEW (dogfood)
```

## Cross-Cutting Design Decisions

1. **No new protobuf for Receptionist.** Receptionist messages use existing
   `TypedMessage` infrastructure with new TypeTags. They are internal-only
   (not wire-transmitted in v1).

2. **Testing utilities are header-only.** `BehaviorTestKit` and `TestProbe`
   live in `include/hpactor/actor/testing/` and are NOT compiled into
   `hpactor_lib`. They follow the `scheduler_test_driver.hpp` precedent.

3. **MessageAdapter is a Behavior combinator.** Follows the pattern established
   in PR #330 (`setup`, `intercept`, `compose`, `on_signal`). Zero changes to
   the `EventBasedActor` dispatch pipeline.

4. **CoordinatedShutdown extends, doesn't replace.** Adds user-defined phase
   slots to the existing `ShutdownCoordinator`. The `ShutdownPhase` enum is
   unchanged; user phases are interleaved with built-in phases by name.

5. **No RTTI, no exceptions.** All components follow HPActor design constraints.

6. **Deterministic tests.** All new tests use `SchedulerTestDriver` or
   synchronous invocation. No wall-clock waits or thread-order assumptions.

---

## 1. Actor Receptionist

### Purpose

Publish/subscribe actor lookup by `ServiceKey<T>` — decouples actor discovery
from specific `ActorId`. Akka equivalent: `akka.actor.typed.receptionist.Receptionist`.

### ServiceKey

Two forms: a concrete runtime `ServiceKey` (stored in maps, sent in messages)
and a typed convenience factory `service_key<T>(name)` for call sites.

```cpp
/// Runtime service key — name + expected message TypeTag.
/// Equality is by name only; the TypeTag is advisory/documentation.
struct ServiceKey {
    std::string name;
    uint32_t type_tag{0};  // expected message TypeTag, 0 = untyped

    bool operator==(const ServiceKey& o) const { return name == o.name; }
    bool operator!=(const ServiceKey& o) const { return name != o.name; }
};

/// Typed convenience factory.
template <typename T>
ServiceKey service_key(std::string_view name) {
    return ServiceKey{std::string(name), T::kTypeTag};
}
```

The `ServiceKey` is not templated — it is a concrete type suitable for use
as a map key and in message payloads. The `service_key<T>(name)` factory
captures the expected message TypeTag at compile time for documentation and
future type-checking. At runtime, registration is by name; actors that
announce the wrong message type for a key produce a log warning.

Hash and equality are by name only:

```cpp
struct ServiceKeyHash {
    size_t operator()(const ServiceKey& k) const {
        return std::hash<std::string>{}(k.name);
    }
};
```

### Messages (internal TypeTags)

| TypeTag | Message | Direction |
|---------|---------|-----------|
| `kReceptionistRegister` | `Register(ServiceKey, ActorId)` | Actor → Receptionist |
| `kReceptionistSubscribe` | `Subscribe(ServiceKey, ActorId)` | Actor → Receptionist |
| `kReceptionistUnregister` | `Unregister(ServiceKey, ActorId)` | Actor → Receptionist |
| `kReceptionistUnsubscribe` | `Unsubscribe(ServiceKey, ActorId)` | Actor → Receptionist |
| `kReceptionistListing` | `Listing(ServiceKey, vector<ActorId>)` | Receptionist → Subscriber |

### Receptionist Actor

Extends `EventBasedActor`. Internal state:

- `unordered_map<ServiceKey, unordered_set<ActorId>> registry_` — registered actors per key
- `unordered_map<ServiceKey, unordered_set<ActorId>> subscribers_` — subscribers per key

Behavior:

- **Register** → add to `registry_[key]`, broadcast `Listing` to all `subscribers_[key]`
- **Unregister** → remove from `registry_[key]`, broadcast updated `Listing`
- **Subscribe** → add to `subscribers_[key]`, immediately reply with current `Listing`
- **Unsubscribe** → remove from `subscribers_[key]`
- **Actor death** — monitors all registered actors; auto-unregisters on death and
  notifies subscribers

### ActorContext API

```cpp
void receptionist_register(ServiceKey key, ActorId self);
void receptionist_unregister(ServiceKey key, ActorId self);
void receptionist_subscribe(ServiceKey key);
void receptionist_unsubscribe(ServiceKey key);
```

Receptionist is a well-known system actor spawned by ActorSystem. A pointer to
it is stored on ActorContext during actor provisioning, similar to how the
scheduler reference is made available.

### GroupRouter Integration

`GroupRouter` gains an alternative constructor:

```cpp
GroupRouter(ServiceKey key, /* routing strategy */);
```

The router subscribes to the Receptionist and dynamically updates its routee
set from `Listing` notifications.

### Deterministic Test Strategy

- Single-threaded `ActorSystem` with `SchedulerTestDriver`
- Register actors, subscribe, verify `Listing` content after drain
- Verify auto-unregister on actor death via `on_exit()`
- Verify group router updates routees dynamically

---

## 2. BehaviorTestKit

### Purpose

Test a `Behavior` synchronously — no `ActorSystem`, no scheduler, no threads.
Send a message, inspect the effects, assert on behavior transitions.
Akka equivalent: `akka.actor.testkit.typed.javadsl.BehaviorTestKit`.

### API

```cpp
class BehaviorTestKit {
public:
    explicit BehaviorTestKit(Behavior behavior);

    template <typename T>
    Effect run(const T& msg);

    const Behavior& current_behavior() const;
    std::optional<TypedMessage> last_reply() const;
    const std::vector<SentMessage>& sent_messages() const;
    const std::vector<SpawnedChild>& spawned_children() const;
    bool is_stopped() const;
};
```

### Effect

```cpp
enum class EffectKind { NoEffect, MessageSent, ReplySent, ChildSpawned, Stopped };

struct Effect {
    EffectKind kind;
};
```

### FakeActorContext

Internal minimal context implementation that:

- Records `send()` → `sent_messages_` vector
- Records `reply()` → `last_reply_`
- Records `spawn()` → `spawned_children_`
- Records `become()` transitions
- Returns a well-known test `ActorId` from `self_address()`

### Constraints

- Header-only (`include/hpactor/actor/testing/behavior_test_kit.hpp`)
- No threads, no ActorSystem, no scheduler
- Covers the 80% use case: validate handler dispatch, behavior transitions,
  sent messages, replies, and child spawn
- Complex lifecycle/supervision tests still use real `ActorSystem`

---

## 3. TestProbe

### Purpose

A lightweight probe actor that receives messages, queues them, and provides
typed assertion helpers. Used with `SchedulerTestDriver` to verify the actor
under test sends the right messages.
Akka equivalent: `akka.actor.testkit.typed.javadsl.TestProbe`.

### API

```cpp
template <typename... MsgTypes>
class TestProbe {
public:
    explicit TestProbe(ActorSystem& system);

    ActorRef ref() const;

    template <typename T>
    const T& expect_message();

    template <typename T>
    void expect_no_message();

    const std::vector<TypedMessage>& queue() const;
    size_t queue_size() const;

    template <typename T, typename Predicate>
    const T& fish_for_message(Predicate pred, int max_items = 100);

    void drain();
};
```

### How It Works

1. Spawns a minimal `EventBasedActor` that appends every received message to a
   `std::vector<TypedMessage>`.
2. `expect_message<T>()` drains the scheduler until the queue is non-empty,
   then asserts the front message is of type `T` and returns it. If the front
   message is a different type, the test fails.
3. `fish_for_message<T>(pred, max)` drains up to `max_items` looking for a
   message of type `T` matching the predicate. Non-matching messages are
   skipped but remain in the queue.
4. `expect_no_message<T>()` asserts the queue contains no messages of type `T`
   (other types are allowed).

### Typed Variant

`TestProbe<Msg1, Msg2>` constrains what messages the probe expects. A template
alias `TestProbe` (untyped) accepts any message type for flexible tests.

### Deterministic Contract

- All drain methods use `SchedulerTestDriver` — no wall-clock waits
- Queue inspection (`queue()`, `queue_size()`) is thread-safe for single-threaded
  test contexts
- Must be used with `ActorSystem` configured with 0 worker threads or paused
  via `SchedulerTestDriver`

---

## 4. MessageAdapter

### Purpose

Translate messages from one protocol to another without spawning intermediate
actors. Two APIs: a `Behavior` combinator for composition, and an
`ActorContext` method that returns a relay `ActorRef`.

Akka equivalent: `context.messageAdapter`.

### Behavior Combinator

```cpp
template <typename From, typename To>
static Behavior message_adapter(
    std::function<To(const From&)> adapter_fn,
    Behavior inner);
```

Wraps `inner` with a dispatch interceptor: messages of type `From` are
translated to `To` via `adapter_fn` before reaching `inner`. All other message
types pass through unchanged.

Follows the existing `ComposeState` dispatch chain established in PR #330.

### ActorContext Method

```cpp
template <typename From, typename To>
ActorRef message_adapter(std::function<To(const From&)> adapter_fn);
```

Returns an `ActorRef` that, when sent a message of type `From`, translates it
to `To` and self-delivers it to this actor. External actors can send to this
ref without knowing the internal protocol.

Internally, this stores a local adapter map on the actor: `unordered_map<ActorId,
MessageAdapterEntry>`. In `receive()`, before dispatching to user handlers,
messages from adapter refs are translated and re-dispatched. This avoids
spawning intermediate relay actors and keeps the adapter overhead to one map
lookup per message from an adapter ref.

### Implementation Footprint

- `Behavior::message_adapter` — ~20 lines in `behavior.hpp`, `ComposeState`
  dispatch entry
- `ActorContext::message_adapter` — ~30 lines, stores adapter functions on the
  context, dispatches on receive

---

## 5. CoordinatedShutdown User-Defined Phases

### Purpose

Let users inject custom shutdown phases between the built-in
`ShutdownPhase` steps. Extends the existing `ShutdownCoordinator` without
replacing it.

Akka equivalent: `akka.actor.CoordinatedShutdown`.

### Current State

`ShutdownCoordinator` has a fixed phase sequence:

```
Running → DrainingIngress → DrainingActors → LeavingCluster →
FlushingTelemetry → Stopped
```

`ShutdownOptions` provides per-phase timeouts. No user extension points.

### New API

```cpp
class ShutdownCoordinator {
public:
    // ... existing API unchanged ...

    /// Register a user phase after a built-in phase.
    /// Returns true on success, false if name already exists.
    bool add_user_phase(
        std::string_view phase_name,
        ShutdownPhase after_phase,
        std::chrono::milliseconds timeout,
        std::function<void()> callback);

    /// Register a user phase after another user-defined phase.
    bool add_user_phase_after(
        std::string_view phase_name,
        std::string_view after_phase_name,
        std::chrono::milliseconds timeout,
        std::function<void()> callback);

    /// Get user-defined phase names in execution order.
    std::vector<std::string_view> user_phase_names() const;
};
```

### Phase Ordering

User phases are interleaved after their declared predecessor:

```
Running
  → DrainingIngress
  → [user phases after DrainingIngress]
  → DrainingActors
  → [user phases after DrainingActors]
  → LeavingCluster
  → [user phases after LeavingCluster]
  → FlushingTelemetry
  → [user phases after FlushingTelemetry]
  → Stopped
```

### Internal Data

```cpp
struct UserPhaseDef {
    std::string name;
    ShutdownPhase after_phase;       // built-in anchor
    std::string after_user_name;     // user-phase anchor (empty if anchored to built-in)
    std::chrono::milliseconds timeout;
    std::function<void()> callback;
};
std::vector<UserPhaseDef> user_phases_;
```

At execution time, `execute()` interleaves user phases with built-in phases by
topologically sorting on the `after_phase`/`after_user_name` dependency edges.

### Failure Semantics

User phases that throw (caught by the coordinator) or exceed their timeout
follow the same `ShutdownOptions::force_after_timeout` force-stop path as
built-in phases. The coordinator logs the phase name and timeout/exception
detail.

### Deterministic Test Strategy

- Build a `ShutdownCoordinator` with injected dependencies
- Register 2 user phases with ordering
- Call `execute()`, verify callbacks fire in correct order
- Verify timeout → force-stop path

---

## Build & Test Impact

| Item | New Files | Modified Files | Test Files |
|------|-----------|---------------|------------|
| Receptionist | 4 (3 headers + 1 src) | `src/CMakeLists.txt`, `actor_context.hpp/cpp` | `test_receptionist.cpp` |
| BehaviorTestKit | 1 (header-only) | None | `behavior_test_kit_test.cpp` |
| TestProbe | 1 (header-only) | None | `test_probe_test.cpp` |
| MessageAdapter | None | `behavior.hpp/cpp`, `actor_context.hpp/cpp` | `test_behavior_message_adapter.cpp` |
| CoordinatedShutdown | None | `shutdown_coordinator.hpp/cpp` | Extended `test_shutdown_coordinator.cpp` |
| GroupRouter | None | `group_router.hpp/cpp` | Extended `test_group_router.cpp` |

### Estimated Test Counts

| Component | Estimated Tests |
|-----------|-----------------|
| Receptionist (register, subscribe, listing, auto-unregister, group-router) | ~20 |
| BehaviorTestKit (run, effects, transitions, FakeContext) | ~15 |
| TestProbe (expect, fish, no-message, typed, queue) | ~15 |
| MessageAdapter (combinator, context relay) | ~10 |
| CoordinatedShutdown (register, ordering, timeout, force-stop) | ~8 |
| **Total** | **~68** |

---

## Out of Scope (Future Sprints)

- Cluster Receptionist (cross-node service key registry)
- BehaviorTestKit effect inspection for timer scheduling
- TestProbe timeout-based waiting (wall-clock mode)
- MessageAdapter for request-response protocols
- CoordinatedShutdown phase dependency DAG (arbitrary DAG ordering)
- Receptionist protobuf wire protocol for cross-node use
- TOML config for Receptionist and user-defined shutdown phases
