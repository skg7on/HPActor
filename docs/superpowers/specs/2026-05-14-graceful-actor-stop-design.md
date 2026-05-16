# Graceful Actor Stop Protocol — Design Spec

**Issue:** [#7 [ACT-002]](https://github.com/skg7on/HPActor/issues/7)
**Date:** 2026-05-14
**Status:** Draft
**Subsystem:** Actor Runtime
**Priority:** P0 (Foundation lane)

## 1. Summary

Add graceful actor stop protocol with per-actor drain policy and timeout, plus
a node-level shutdown coordinator. Actors can drain their mailbox, drop user
messages to the existing dead-letter queue, or stop immediately, with a timeout
that prevents indefinite blocking. The node shutdown coordinator drives the
full shutdown phase machine: gating ingress, draining actors in reverse
topological order, leaving the cluster, and flushing telemetry.

## 2. Motivation

From the production reliability plane and issue #7:

- Actors need stop policies that decide what happens to mailbox contents and
  in-flight work during rolling upgrades and controlled shutdown.
- Stuck actors must not block node shutdown indefinitely.
- Dropped messages must be dead-lettered with reason for debugging and
  operational visibility.
- Operators need CLI visibility into shutdown progress.

## 3. Design

### 3.1 Drain Policy

```cpp
enum class DrainPolicy : uint8_t {
    Drain = 0,            // Process all mailbox messages before stopping
    DropUserMessages = 1, // Dead-letter user messages, keep system messages
    ImmediateStop = 2,    // Stop immediately, dead-letter everything
    SnapshotAndStop = 3,  // [DEFERRED] Durable actors persist state then stop
    TransferShard = 4,    // [DEFERRED] Sharded actors hand off ownership
};
```

`SnapshotAndStop` and `TransferShard` are enum stubs only — they are recognized
but deferred until durable actor state and cluster sharding are implemented
(respectively). Attempting to use them logs a warning and falls back to `Drain`.

**Policy behavior during drain:**

| Policy | User messages | System messages | On timeout |
|--------|-------------|----------------|------------|
| `Drain` | Process all | Process all | Force stop, DLQ remainder |
| `DropUserMessages` | Dead-letter, skip | Process all | Force stop |
| `ImmediateStop` | Dead-letter all | Dead-letter all | N/A (instant) |

System messages (`LinkMsg`, `UnlinkMsg`, `DownMsg`, `InspectStateRequest`,
`KillRequest`, `SystemInitTag`, etc.) always bypass user-message rejection
during drain — they are required for shutdown coordination.

### 3.2 Per-Actor Configuration

```cpp
struct DrainConfig {
    DrainPolicy policy{DrainPolicy::Drain};
    std::chrono::milliseconds timeout{30'000};
};
```

Stored as a protected member `drain_config_` on `LifecycleActor`, with public
getter/setter:

```cpp
// On LifecycleActor:
DrainConfig drain_config() const noexcept { return drain_config_; }
void set_drain_config(DrainConfig cfg) noexcept { drain_config_ = cfg; }
```

System-wide defaults in `[system.shutdown]` TOML section, parsed by a
self-registering subsystem parser (`src/config/parsers/shutdown_config_parser.cpp`).
Per-actor overrides via topology actor args (e.g., `drain_policy = "DropUserMessages"`,
`drain_timeout_ms = 10000`). `ActorSystem::set_drain_config(ActorId, DrainConfig)`
provides programmatic per-actor override from outside the actor.

### 3.3 Drain Execution Flow

**State machine** — uses the existing `LifecycleActor` transitions:

```
Active → Draining → Stopping → Stopped
  │                    │
  └────────────────────┘  (ImmediateStop skips Draining, goes Active→Stopping)
```

Note: `Active→Stopping` is already a legal transition in `kStateMachine`
(see `lifecycle_state.hpp` line 41). `ImmediateStop` uses this path.

**Drain loop:**

1. Stop triggered via `ActorContext::stop()` or `ActorSystem::shutdown()`.
2. `transition(kDraining)` succeeds → `on_drain()` hook invoked.
3. A one-shot timer is scheduled on the `TimingWheel` for `drain_config_.timeout`.
   The callback invokes the new `on_drain_timeout()` hook.
4. Mailbox iteration:
   - **Drain**: dequeue and process each message normally (user + system).
   - **DropUserMessages**: dequeue; user messages → `system().dead_letter(record)`;
     system messages → process.
   - **ImmediateStop**: skip drain entirely; dead-letter all remaining messages
     via `system().dead_letter()`; go straight to Stopping.
5. Drain completes when mailbox is empty → cancel timer → `transition(kStopping)`.
6. If timer fires before drain completes → `on_drain_timeout()` hook invoked →
   remaining messages dead-lettered → force `transition(kStopping)`.

**New virtual hook on LifecycleActor:**

```cpp
virtual void on_drain_timeout() {}  // called when drain deadline expires
```

**Stopping phase:**

- `on_stop()` hook — unlink from peers, cancel timers, close resources.
- `transition(kStopped)` → `on_deactivate()` — final cleanup, mailbox dropped.
- `DownMsg` sent to all linked and monitoring actors (existing mechanism in
  `EventBasedActor::on_exit()`).

**Message gate during drain:**

- `accepts_user_msgs()` already returns `false` for `kDraining` in the existing
  state table (`lifecycle_state.hpp` line 44).
- New user messages arriving during drain are rejected at the gate and
  dead-lettered with `DeadLetterReason::MailboxClosed`.
- System messages are always accepted during drain states (confirmed by
  `accepts_system_msgs: true` in `kStateMachine` for `kDraining`).

**Coroutine actor note:** The drain loop operates at the mailbox level, not the
scheduler dispatch level. For coroutine actors suspended via `co_await
mailbox_awaiter`, the drain loop resumes the coroutine to process each message,
and the coroutine re-suspends when the mailbox is exhausted. The drain is
complete when the mailbox is empty AND the coroutine is not in-flight (no
pending resumption). The same timeout guard applies.

### 3.4 Stop API

#### LifecycleActor additions

```cpp
// New members on LifecycleActor:
virtual void on_drain_timeout() {}   // drain deadline expired
DrainConfig drain_config() const noexcept;
void set_drain_config(DrainConfig cfg) noexcept;

protected:
    DrainConfig drain_config_{};
```

#### ActorContext — Individual Actor Stop

```cpp
// Async stop — initiates drain, returns immediately.
// Actor transitions through Draining→Stopping→Stopped on its scheduler.
void ActorContext::stop(ActorId target);

// Sync stop with timeout — blocks until Stopped or deadline.
// Returns error on timeout. Only callable from non-actor threads or BlockingActor.
result<void> ActorContext::stop_sync(ActorId target, milliseconds timeout);
```

`ActorContext::stop()` can stop any actor, but is primarily for parents stopping
children or self-stop. For cross-actor stop (e.g., admin stopping an arbitrary
actor), the caller sends a system `StopRequest` message; the target actor's
behavior handles it by calling `context()->stop(self_id)`.

#### ActorSystem additions

```cpp
// Per-actor drain config override (for external/admin use)
void ActorSystem::set_drain_config(ActorId target, DrainConfig cfg);

// Readiness flag for health gating
bool ActorSystem::is_ready() const noexcept;   // false once DrainingIngress starts
bool ActorSystem::is_draining() const noexcept; // true during DrainingActors phase
```

#### ActorSystem — Node Shutdown

```cpp
enum class ShutdownPhase : uint8_t {
    Running,
    DrainingIngress,      // Gate external traffic
    DrainingActors,       // Drain all actors
    LeavingCluster,       // Advertise leaving, reject new cluster traffic
    FlushingTelemetry,    // Flush logs, metrics, traces, DLQ
    Stopped,
    ForcedStop,           // Timeout exceeded
};

struct ShutdownOptions {
    milliseconds ingress_timeout{5'000};
    milliseconds actor_drain_timeout{30'000};
    milliseconds cluster_leave_timeout{10'000};
    bool force_after_timeout{true};
};

result<void> ActorSystem::shutdown(const ShutdownOptions& opts);
```

#### Shutdown Phase Machine

```
Running → DrainingIngress → DrainingActors → LeavingCluster → FlushingTelemetry → Stopped
              │                   │                  │                   │
              └───────────────────┴──────────────────┴───────────────────┘
                               ForcedStop (timeout)
```

Phase behavior:

- **DrainingIngress**: HTTP gateways, remote spawn, admin mutating commands
  reject new user work. `is_ready()` becomes `false`.
- **DrainingActors**: all actors drained in reverse topological order (children
  before parents, leaves before roots). System actors drain last. Topological
  order is derived via post-order traversal of the spawn tree using
  `ActorContext::children()` queries. System actors are identified by a new
  virtual `is_system_actor()` returning `true` (default `false`); system actors
  override this to defer their drain until after all user actors.
- **LeavingCluster**: discovery advertises `Leaving` membership state. New
  cluster traffic is rejected. Shard handoff is deferred until cluster sharding
  is implemented — in this PR, only the membership advertisement and traffic
  rejection are implemented.
- **FlushingTelemetry**: logs, metrics, traces, DLQ export complete best-effort.
- **ForcedStop**: entered when any phase exceeds its deadline and
  `force_after_timeout` is true. Remaining actors are terminated immediately
  (`ImmediateStop`).

Each phase has a deadline derived from `ShutdownOptions`. The `ShutdownCoordinator`,
a short-lived internal object (not an actor), drives the phase machine on the
calling thread.

#### CLI Commands

```text
/system drain                    — trigger full shutdown
/system drain status             — show phase, pending count, deadlines
/system stop <actor-id>          — graceful stop individual actor (drains per policy)
/system stop <actor-id> --force  — ImmediateStop an actor
```

Relationship to existing `/actor <id> kill`:
- `/actor <id> kill` uses the existing `KillRequest` system message path
  (immediate termination). It is simpler but does not honor drain policy.
- `/system stop <actor-id>` is the new graceful-stop CLI surface that respects
  per-actor drain policy and timeout.
- Both coexist: `kill` for immediate force-stop, `stop` for graceful drain.

### 3.5 Dead Letter Queue Integration

The DLQ is already implemented (`mailbox::DeadLetterQueue` in
`include/hpactor/mailbox/dead_letter_queue.hpp`). Messages are written via
`ActorSystem::dead_letter(record)`. The drain policies use this existing API.

**New DeadLetterReason values to add:**

Two new values are added to the existing `mailbox::DeadLetterReason` enum:

```cpp
enum class DeadLetterReason : uint8_t {
    // ... existing values ...
    MailboxClosed,          // existing — reused for messages arriving after kStopped
    NoDropRejected,         // existing — reused for policy-driven rejection
    // NEW:
    DrainTimeout = 12,      // message dropped because drain deadline expired
    DrainPolicyDrop = 13,   // message dropped by DropUserMessages policy
};
```

**Mapping of drain scenarios to DeadLetterReason:**

| Scenario | DeadLetterReason |
|----------|-----------------|
| User message dropped by `DropUserMessages` policy | `DrainPolicyDrop` |
| Message dropped on drain timeout | `DrainTimeout` |
| Message arriving after `kStopped` | `MailboxClosed` (existing) |
| All messages dropped by `ImmediateStop` | `MailboxClosed` (existing) |
| New user message arriving during `kDraining` | `MailboxClosed` (existing) |

**DeadLetterSource:** All drain-generated DLQ records use
`DeadLetterSource::MailboxAdmission` (existing value).

### 3.6 Observability

**Metrics** — added to existing `MetricEvent` system:

| Metric | Type | Description |
|--------|------|-------------|
| `hpactor_actor_drain_policy` | Gauge | Per-actor drain policy (labels: `actor_type`, `policy`) |
| `hpactor_actor_drain_duration_ms` | Gauge | Time spent in Draining state (computed by aggregator from start/complete events) |
| `hpactor_actor_drain_timeouts_total` | Counter | Actors that timed out during drain |
| `hpactor_actor_drain_messages_dlq_total` | Counter | Messages dead-lettered during drain |
| `hpactor_shutdown_phase` | Gauge | Current node shutdown phase (0-6) |
| `hpactor_shutdown_duration_seconds` | Gauge | Total node shutdown duration |
| `hpactor_shutdown_forced_total` | Counter | Forced stops due to timeout |

Note: Histogram-style metrics (duration distributions) are approximated via the
existing `MetricRegistry` Gauge and Counter types, plus the aggregator computing
min/max/avg from raw event streams. Full histogram support in `MetricRegistry`
is a separate concern.

**Metric events** — new values added to `MetricEventType` enum:
- `kActorDrainStart` (emitted on `transition(kDraining)`)
- `kActorDrainComplete` (emitted on `transition(kStopping)` after successful drain)
- `kActorDrainTimeout` (emitted when drain timer fires)

**Logging** — via existing structured logging. New `LogEventId` values:

| ID | Name | Level | Description |
|----|------|-------|-------------|
| 1004 | `kActorDrainStart` | debug | Per-actor drain started |
| 1005 | `kActorDrainComplete` | debug | Per-actor drain completed |
| 1006 | `kActorDrainTimeout` | warn | Drain deadline expired |
| 1007 | `kShutdownPhaseTransition` | info | Node shutdown phase change |

**CLI visibility:**
```text
/actor <id> show  — includes drain_policy, drain_status, drain_deadline
/system drain status  — pending count, per-actor progress, phase deadlines
```

## 4. Dependencies

- **Existing (already implemented):** `LifecycleActor` mixin with `kDraining`/
  `kStopping`/`kStopped` states and CAS-based `transition()`, `TimingWheel` for
  drain timeout scheduling, `mailbox::DeadLetterQueue` with `ActorSystem::dead_letter()`,
  `MpscRingBuffer` for metrics events, structured logging with `LogEventId`,
  TOML config subsystem with self-registering parsers, CLI trie-based command tree,
  `DownMsg` propagation for linked/monitored actors.
- **Follow-up:** Durable actor state (for `SnapshotAndStop`), cluster sharding
  (for `TransferShard` and `LeavingCluster` shard handoff), full `Histogram`
  support in `MetricRegistry`.

## 5. Non-Goals

- Infinite wait for stuck actors (timeout guarantees forward progress).
- Transparent live code replacement inside an actor.
- Cross-version protocol compatibility (separate feature).
- Implementing `SnapshotAndStop` or `TransferShard` beyond enum stubs.
- Full cluster leave with shard handoff (deferred until sharding is built).
- Full histogram/bucket support in MetricRegistry (use Gauge + aggregator).

## 6. API Surface Summary

All new public API introduced by this feature:

| API | Location | Purpose |
|-----|----------|---------|
| `enum class DrainPolicy` | new header `actor/drain_policy.hpp` | Per-actor drain behavior |
| `struct DrainConfig` | new header `actor/drain_config.hpp` | Policy + timeout pair |
| `LifecycleActor::on_drain_timeout()` | `actor/lifecycle_actor.hpp` | Virtual hook for timeout |
| `LifecycleActor::drain_config()` / `set_drain_config()` | `actor/lifecycle_actor.hpp` | Per-actor config accessors |
| `LifecycleActor::drain_config_` (protected) | `actor/lifecycle_actor.hpp` | Config storage |
| `ActorContext::stop(ActorId)` | `core/actor_context.hpp` | Async graceful stop |
| `ActorContext::stop_sync(ActorId, ms)` | `core/actor_context.hpp` | Sync graceful stop |
| `ActorSystem::set_drain_config(ActorId, cfg)` | `core/actor_system.hpp` | Admin config override |
| `AbstractActor::is_system_actor()` | `actor/abstract_actor.hpp` | Virtual, default false; true for system actors drained last |
| `ActorSystem::is_ready()` / `is_draining()` | `core/actor_system.hpp` | Health/readiness gating |
| `ActorSystem::shutdown(ShutdownOptions)` | `core/actor_system.hpp` | Node shutdown |
| `enum class ShutdownPhase` | `core/actor_system.hpp` | Shutdown phase enum |
| `struct ShutdownOptions` | `core/actor_system.hpp` | Shutdown parameters |
| `DeadLetterReason::DrainTimeout` (12) | `mailbox/dead_letter_queue.hpp` | New DLQ reason |
| `DeadLetterReason::DrainPolicyDrop` (13) | `mailbox/dead_letter_queue.hpp` | New DLQ reason |
| `MetricEventType::kActorDrainStart` | `metrics/metrics_event.hpp` | New metric event |
| `MetricEventType::kActorDrainComplete` | `metrics/metrics_event.hpp` | New metric event |
| `MetricEventType::kActorDrainTimeout` | `metrics/metrics_event.hpp` | New metric event |
| CLI: `/system drain`, `/system drain status`, `/system stop` | `cli/cli_actor.cpp` | New commands |
| `LogEventId::kActorDrainStart` (1004) | `log/detail/log_macros.hpp` | New log event |
| `LogEventId::kActorDrainComplete` (1005) | `log/detail/log_macros.hpp` | New log event |
| `LogEventId::kActorDrainTimeout` (1006) | `log/detail/log_macros.hpp` | New log event |
| `LogEventId::kShutdownPhaseTransition` (1007) | `log/detail/log_macros.hpp` | New log event |

## 7. Acceptance Criteria

- [ ] `Drain`, `DropUserMessages`, and `ImmediateStop` policies exist and are
  functional.
- [ ] `SnapshotAndStop` and `TransferShard` are valid enum values that fall back
  to `Drain` with a warning log.
- [ ] Per-actor drain timeout prevents indefinite blocking; timer fires, calls
  `on_drain_timeout()`, and forces transition to Stopping.
- [ ] Dropped messages are written to the existing DLQ via
  `ActorSystem::dead_letter()` with correct `DeadLetterReason`.
- [ ] `ActorContext::stop()` and `ActorContext::stop_sync()` work for individual
  actor stop.
- [ ] `ActorSystem::shutdown()` drives the full phase machine with per-phase
  timeouts and `ForcedStop` fallback.
- [ ] System messages bypass user-message rejection during drain.
- [ ] `DownMsg` sent to linked/monitoring actors on stop.
- [ ] CLI commands: `/system drain`, `/system drain status`, `/system stop
  <id>` (with `--force` flag).
- [ ] TOML `[system.shutdown]` config with per-actor overrides in topology.
- [ ] Metrics, logs, and CLI expose drain/shutdown state.
- [ ] `is_ready()` returns `false` once `DrainingIngress` starts.
- [ ] `is_system_actor()` virtual method identifies actors drained last.

## 8. Test Plan

| Test Suite | Coverage |
|-----------|----------|
| `test_drain_policy` | Per-policy mailbox handling (Drain, DropUserMessages, ImmediateStop), deferred policy fallback, DLQ record verification |
| `test_drain_timeout` | Timer fires, `on_drain_timeout()` invoked, transition forced, messages dead-lettered |
| `test_actor_stop` | `ActorContext::stop()` and `stop_sync()`, DownMsg propagation, message gate during drain |
| `test_shutdown_coordinator` | Phase machine transitions, per-phase deadlines, ForcedStop, topological ordering |
| `test_shutdown_config` | TOML parsing, system defaults, per-actor override, `set_drain_config()` |
| `test_drain_integration` | End-to-end: spawn tree → node shutdown → all actors stopped, system actors drain last |
