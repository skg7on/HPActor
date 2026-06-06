# ACT-008: Actor Passivation with Recovery and Route Handling — Design Spec

**Issue**: [#13](https://github.com/skg7on/HPActor/issues/13)
**Subsystem**: Actor Runtime
**Priority**: P2
**Release lane**: Durability
**Backlog source**: `docs/architecture/production/architecture-requirement-backlog.md#L32`
**Architecture doc**: `docs/architecture/production/durable-actor-state-design.md`

## 1. Executive Summary

HPActor already has memory-level actor hibernation: the `Hibernatable` interface
serializes actor state to a buffer, the `HibernationRegistry` stores it, and
`HibernationManager` uses `madvise`/`munmap` to release cold pages. But this is
a pure memory-management concern — there is no lifecycle integration, no idle
detection, no durable storage, no route handling for messages that arrive while
the actor is hibernated, and no operator visibility.

ACT-008 extends hibernation into production passivation:

- **Lifecycle integration**: two new lifecycle states (`kPassivating`,
  `kPassivated`) with full transition-table validation.
- **Four trigger paths**: idle timeout, explicit self-request, memory pressure,
  and CLI/admin.
- **Durable storage**: a new `IDurableActor` interface and `DurableStateStore`
  abstraction with `InMemoryStateStore` and `FileStateStore` implementations.
- **Route stubs**: a `LocalPassivatedRoute` that buffers messages and triggers
  lazy reactivation, built behind an `IActorRoute` interface that is
  forward-compatible with future sharding.
- **Full observability**: metrics, structured logging, CLI commands, DLQ
  integration, and failure-reason codes.

## 2. What Already Exists

### 2.1 Memory-Level Hibernation (✅ Complete)

| Item | Location | Notes |
|------|----------|-------|
| `Hibernatable` | `mem/hibernatable.hpp` | `serialized_size()`, `serialize_to()`, `deserialize_from()` — pure memory buffering |
| `HibernationRegistry` | `mem/hibernation_registry.hpp` | Singleton, mutex-protected `ActorId → HibernationBuffer` map |
| `HibernationManager` | `src/mem/hibernation_manager.cpp` | `munmap`/`madvise` to release cold memory |
| Memory region `kHibernate` | `mem/memory_region.hpp` | Separate accounting region for hibernation buffers |
| Telemetry events | `mem/telemetry_ring_buffer.hpp` | `kHibernateIn` (3), `kHibernateOut` (4) |
| Tests | `tests/unit/mem/test_hibernation.cpp` | 2 tests: store/load cycle, interface serialization |

### 2.2 Lifecycle State Machine (✅ Complete)

| Item | Location | Notes |
|------|----------|-------|
| `LifecycleState` enum | `actor/lifecycle_state.hpp` | 8 states: `kStarting` through `kQuarantined` |
| `StateDef` table | `actor/lifecycle_state.hpp` | Constexpr transition table with 8 entries |
| `LifecycleActor` | `actor/lifecycle_actor.hpp` | CAS-based `transition()`, virtual hooks per state |
| `LifecycleActor::transition()` | `src/actor/lifecycle_actor.cpp` | Validates + CAS + invokes hook |

Current state machine (for reference):
```
kStarting → kActive, kFailed
kActive → kDraining, kStopping, kFailed, kQuarantined
kDraining → kStopping, kFailed
kStopping → kStopped, kFailed
kStopped → kStarting
kFailed → kStarting, kStopped, kRecovering, kQuarantined
kRecovering → kActive, kFailed, kQuarantined
kQuarantined → kStopped
```

`kRecovering` exists only as a recovery path from `kFailed` — it is currently
used for durability recovery (snapshot/event replay) after a failure, not for
passivation reactivation.

### 2.3 AbstractActor Serialization Hook

`AbstractActor::serialize_state()` returns an empty vector by default. Stateful
actors override this for hibernation. The hook exists but has no lifecycle
integration or durable storage.

### 2.4 Drain and Shutdown (✅ Complete)

| Item | Location | Notes |
|------|----------|-------|
| `DrainConfig` | `actor/drain_config.hpp` | Per-actor drain policy and timeout |
| `DrainPolicy` | enum | `Complete`, `Drop`, `Timeout` |
| Drain timeout | `TimingWheel` | Configurable deadline for in-flight message completion |
| `ActorSystem::shutdown()` | `core/actor_system.hpp` | Phase-machine coordinator |

The existing drain infrastructure is reused for the passivation drain phase.

### 2.5 Production Architecture Docs

| Document | Relevant Content |
|----------|-----------------|
| `durable-actor-state-design.md` §8 | Passivation lifecycle: stop accepting → drain/reroute → persist snapshot → release memory → keep route → lazy reactivation |
| `feature-gap-refined-requirement-backlog.md` §ACT-008 | Idle detection, drain/DLQ policy, final snapshot, route owned by shard, lazy reactivation, recovery before message handling |
| `architecture-requirement-backlog.md` L32 | ACT-008: "Extend hibernation into production passivation with recovery and route handling" |

## 3. Design

### 3.1 Architecture Overview

```
                       ┌─────────────────────────────────────────┐
                       │           Actor Lifecycle                │
                       │                                         │
  ┌────────┐           │  Active ──► Passivating ──► Passivated  │
  │ Idle   │──────────►│    │           (drain +       (route     │
  │ Timer  │  trigger   │    │           snapshot)      stub)     │
  └────────┘           │    │              │                │    │
                       │    │              │                │    │
  ┌────────┐           │    │              ▼                ▼    │
  │ Self-  │──────────►│  Reactivated ◄── Recovering ◄── Message │
  │ request│  trigger   │    ▲          (restore)        arrives  │
  └────────┘           │    │                                     │
                       │    └── Failed (recovery failure)         │
  ┌────────┐           │                                         │
  │ Memory │──────────►│  Route Stub:  ActorId + metadata only   │
  │Pressure│  trigger   │  ┌──────────────────────────────────┐   │
  └────────┘           │  │ IActorRoute (interface)           │   │
                       │  │  ├── LocalActiveRoute (live)      │   │
  ┌────────┐           │  │  ├── LocalPassivatedRoute (stub)  │   │
  │  CLI   │──────────►│  │  └── ShardOwnedRoute (future)     │   │
  │/passiv.│  trigger   │  └──────────────────────────────────┘   │
  └────────┘           └─────────────────────────────────────────┘
```

### 3.2 Lifecycle State Machine Changes

Two new states added to `LifecycleState`:

```cpp
enum class LifecycleState : uint8_t {
    kStarting    = 0,
    kActive      = 1,
    kDraining    = 2,
    kStopping    = 3,
    kStopped     = 4,
    kFailed      = 5,
    kRecovering  = 6,
    kQuarantined = 7,
    kPassivating = 8,  ///< Draining + snapshotting, rejects user msgs
    kPassivated  = 9,  ///< Memory freed, route stub alive, durable state stored
};
```

New state definitions in the `kStateMachine` table:

| State | accepts_user_msgs | accepts_system_msgs | Valid transitions |
|-------|-------------------|---------------------|-------------------|
| `kPassivating` | false | true | `kPassivated`, `kFailed` |
| `kPassivated` | false | true | `kRecovering`, `kStopped`, `kFailed` |

New transitions added to existing states:
- `kActive` gains: `→ kPassivating`
- `kRecovering` gains: entry from `kPassivated` (existing path to `kActive`/`kFailed`/`kQuarantined` unchanged)

Full transition diagram with new states:

```
kStarting ──► kActive ──► kPassivating ──► kPassivated ──► kRecovering ──► kActive
   │             │              │                │                │
   ▼             ▼              ▼                ▼                ▼
kFailed       kDraining      kFailed          kStopped         kFailed
   │             │                             (shutdown)         │
   ▼             ▼                                               ▼
kStopped      kStopping                                    kQuarantined
                 │
                 ▼
              kStopped
```

New virtual hooks on `LifecycleActor`:
- `virtual void on_passivating() {}` — invoked after transition to `kPassivating`
- `virtual void on_passivated() {}` — invoked after transition to `kPassivated`

### 3.3 Passivation Protocol

#### Phase 1: Enter Passivating (`Active → Passivating`)

Trigger fires → `LifecycleActor::transition(kPassivating)`:

1. State changes to `kPassivating`. Message gate rejects user messages (system
   messages — link/unlink, shutdown, CLI inspect — still accepted).
2. `on_passivating()` hook invoked. Default implementation starts draining the
   actor's mailbox using the actor's existing `DrainConfig`.
3. If the actor is durable (implements `IDurableActor`), the drain processes all
   queued messages normally to ensure state consistency before snapshotting.

#### Phase 2: Snapshot and Persist

After the last queued message is processed:

1. If the actor implements `IDurableActor`:
   - Call `snapshot_state()` to serialize current state.
   - Write the snapshot via `DurableStateStore::write_snapshot()` with the
     actor's persistence ID, current schema version, and serialized state.
     The store assigns the next monotonic sequence number and timestamp.
2. If the actor implements `Hibernatable` (non-durable, memory-only
   passivation):
   - Call `serialize_to()` and hand the buffer to
     `HibernationRegistry::store()`.
3. An actor may implement both: `IDurableActor` takes precedence for
   passivation; `Hibernatable` remains available for non-passivation memory
   management.
4. On snapshot/store failure: `transition(kFailed)` with
   `FailureReason::PassivationSnapshotFailed`.
5. On drain timeout: `transition(kFailed)` with
   `FailureReason::PassivationDrainTimeout`.

#### Phase 3: Release (`→ Passivated`)

1. `on_passivated()` hook invoked. Default implementation releases actor memory
   — the actor object may be deallocated, and its coroutine stack/frame returned
   to the pool.
2. A lightweight `LocalPassivatedRoute` is registered in the actor registry,
   replacing the full actor entry. It holds: `ActorId`, `persistence_id`,
   `PassivationRecord` (timestamp, schema version, snapshot sequence), and a
   bounded reactivation buffer.
3. Lifecycle state is `kPassivated`. The route stub owns the lifecycle state
   from this point forward — there is no live actor object.

### 3.4 Reactivation Protocol

A message arrives targeting a passivated actor:

1. `LocalPassivatedRoute::try_deliver()` buffers the message in its reactivation
   queue (bounded, configurable capacity, default 64).
2. It sets an atomic `reactivation_in_progress` flag and spawns a recovery task
   on the scheduler. Subsequent messages see the flag already set and simply
   enqueue to the buffer.
3. State transitions `Passivated → Recovering`.
4. If durable: load latest snapshot from `DurableStateStore`, construct a new
   actor instance, call `IDurableActor::restore_snapshot()`, then replay events
   after the snapshot sequence via `IDurableActor::apply_event()`.
5. If memory-only: load buffer from `HibernationRegistry`, allocate new actor
   instance, call `Hibernatable::deserialize_from()`.
6. On success: `Recovering → Active`. The actor instance replaces the route stub
   in the registry. Buffered messages are drained into the new actor's mailbox
   in order.
7. On failure: `Recovering → Failed`. Buffered messages are dead-lettered with
   `DeadLetterReason::ReactivationFailed`. The supervisor handles restart or
   quarantine per its policy.
8. If the reactivation queue fills before reactivation completes,
   `try_deliver()` returns `EnqueueResult::Rejected` with
   `FailureReason::PassivationQueueFull`. The sender receives this as a
   retryable failure.

#### Shutdown During Passivation

If the system shuts down while an actor is `kPassivated`:
- `Passivated → Stopped` transition. The route stub invokes
  `DurableStateStore::delete_state()` if durable, or
  `HibernationRegistry::remove()` if memory-only. No reactivation needed.

### 3.5 Passivation Triggers

| Trigger | Mechanism | Config |
|---------|-----------|--------|
| Idle timeout | `TimingWheel` timer reset on each message processed; fires after `idle_timeout` of inactivity | `PassivationConfig::idle_timeout` (0 = disabled) |
| Self-request | `ActorContext::passivate()` — actor calls after completing a logical unit of work | N/A |
| Memory pressure | `MemoryPressureMonitor` selects least-recently-used passivatable actors when system memory exceeds high watermark | `PassivationConfig::allow_memory_pressure` (default true) |
| CLI/admin | `/actor passivate <id>` — operator-driven, immediate | Requires CLI enabled |

#### Idle Timeout

The idle timer is managed by the scheduler's `TimingWheel`. Each time the actor
processes a user message, the timer is reset to `idle_timeout` from the current
time. System messages (link/unlink, CLI inspect, supervision) do not reset the
timer. When the timer fires, `on_idle()` is called on the actor's scheduler
thread, which invokes `transition(kPassivating)`.

If `idle_timeout` is 0, the idle timer is never armed and this trigger is
disabled.

#### Self-Request

```cpp
// From within an actor's message handler:
context()->passivate(); // schedules passivation after current message completes
```

The passivation is deferred until the current message handler returns, ensuring
the actor is in a consistent state for snapshotting.

#### Memory Pressure

`MemoryPressureMonitor` is a new subsystem owned by `ActorSystem`:

- Polls system memory usage periodically (configurable interval, default 5s).
- When usage exceeds `memory_pressure_high_threshold_pct` (default 85%):
  1. Scans the actor registry for actors in `kActive` state with
     `allow_memory_pressure = true` and an idle duration above a minimum
     threshold.
  2. Selects actors in LRU order up to a configurable batch size.
  3. Invokes `transition(kPassivating)` on each selected actor.
- Stops selecting when memory drops below the threshold.

#### CLI/Admin

```
/actor passivate <id>         # Immediate passivation
/actor reactivate <id>        # Immediate reactivation (sends a system wakeup)
/actor list passivated        # All passivated actors
```

### 3.6 Route Handling

#### `IActorRoute` Interface

An abstraction over "where messages go" that decouples the sender from whether
the actor is active, passivated, or remote:

```cpp
class IActorRoute {
public:
    virtual ~IActorRoute() = default;

    /// Attempt delivery. Returns the enqueue result.
    virtual EnqueueResult try_deliver(TypedMessage msg) = 0;

    /// Whether this route currently accepts messages.
    virtual bool is_active() const = 0;

    /// The lifecycle state of the target actor (or its proxy).
    virtual LifecycleState state() const = 0;

    /// Human-readable description for CLI/debug.
    virtual std::string describe() const = 0;
};
```

#### `LocalActiveRoute`

Wraps a live `LocalActor*`. Delegates `try_deliver()` directly to the actor's
mailbox. This formalizes what `ActorRef`/`ActorProxy` already does implicitly.
Introduced to provide a uniform `IActorRoute` interface for the registry.

#### `LocalPassivatedRoute`

Replaces the actor entry in the registry when an actor passivates:

| Field | Type | Purpose |
|-------|------|---------|
| `actor_id` | `ActorId` | Stable identity |
| `persistence_id` | `std::string` | Links to `DurableStateStore` |
| `passivated_at` | `steady_clock::time_point` | Monotonic timestamp |
| `schema_version` | `uint32_t` | For recovery compatibility check |
| `snapshot_sequence` | `uint64_t` | Latest snapshot sequence number |
| `reactivation_queue` | `MpscRingBuffer<TypedMessage>` | Bounded buffer, configurable capacity (default 64) |
| `reactivation_in_progress` | `std::atomic<bool>` | Prevents duplicate reactivation attempts |

`try_deliver()` behavior:

1. If `reactivation_in_progress` is already true, enqueue to
   `reactivation_queue`. If the queue is full, return
   `EnqueueResult::Rejected` with `FailureReason::PassivationQueueFull`.
2. Otherwise, set the flag to true, enqueue the triggering message, and spawn a
   reactivation task on the scheduler.
3. The reactivation task runs the Recovering → Active protocol.
4. On success, the route upgrades itself to `LocalActiveRoute` and drains the
   buffered messages into the new actor's mailbox in FIFO order.
5. On failure, buffered messages are dead-lettered with
   `DeadLetterReason::ReactivationFailed`.

#### `ShardOwnedRoute` (Future)

Designed but implemented when sharding lands (CLS-003). Holds a `ShardId` +
`NodeId` reference. `try_deliver()` forwards to the shard owner, which either
reactivates the local passivated copy or forwards to the node that owns the
active shard. The `IActorRoute` interface means no sender code changes when
this is added.

#### Route Lifecycle in the Registry

```
spawn:          registry entry = LocalActiveRoute(actor*)
passivate:      registry entry = LocalPassivatedRoute(metadata)
reactivate:     registry entry = LocalActiveRoute(new actor*)
terminate:      registry entry removed, durable state cleaned up
```

### 3.7 Durable State Storage

#### `IDurableActor` Interface

A new opt-in interface for actors that want durable passivation (vs.
memory-only via `Hibernatable`):

```cpp
class IDurableActor {
public:
    virtual ~IDurableActor() = default;

    /// Stable identity across passivation/restart cycles.
    virtual std::string_view persistence_id() const = 0;

    /// Serialize current state for a snapshot.
    /// Called during passivation. Must not block.
    virtual result<StreamBuffer> snapshot_state() const = 0;

    /// Restore state from a snapshot.
    /// Called during reactivation, before any messages are delivered.
    virtual result<void> restore_snapshot(const StreamBuffer& data) = 0;

    /// Apply a persisted event to in-memory state.
    /// For event-sourced actors. Default is no-op.
    virtual result<void> apply_event(const StreamBuffer& event) {
        return success();
    }

    /// Migrate a snapshot from an older schema version.
    ///
    /// Called during recovery when the stored schema_version differs from
    /// the actor's current version. The default returns
    /// FailureReason::SchemaVersionMismatch. Actors with version history
    /// override this to provide upgrade chains.
    ///
    /// \param[in] from_version The schema version of the stored snapshot.
    /// \param[in] data          The snapshot payload in the old format.
    /// \return The migrated payload in the current schema format, or an error.
    virtual result<StreamBuffer> migrate_snapshot(
        uint32_t /*from_version*/, const StreamBuffer& /*data*/) {
        return error::make(FailureReason::SchemaVersionMismatch);
    }
};
```

#### `DurableStateStore` Interface

Abstracts the persistence backend:

```cpp
class DurableStateStore {
public:
    virtual ~DurableStateStore() = default;

    // Snapshot operations
    virtual result<SnapshotRecord> write_snapshot(
        std::string_view persistence_id,
        uint32_t schema_version,
        StreamBuffer data) = 0;

    virtual result<SnapshotRecord> load_latest_snapshot(
        std::string_view persistence_id) = 0;

    // Event operations (for event-sourced actors)
    virtual result<void> append_event(std::string_view persistence_id,
                                      uint64_t sequence,
                                      StreamBuffer event) = 0;

    virtual result<std::vector<EventRecord>> load_events_after(
        std::string_view persistence_id,
        uint64_t after_sequence) = 0;

    // Lifecycle
    virtual result<void> delete_state(std::string_view persistence_id) = 0;
};
```

#### Data Records

```cpp
struct SnapshotRecord {
    std::string persistence_id;
    uint64_t sequence;
    uint32_t schema_version;
    uint32_t serializer_id;
    uint64_t timestamp_ms;       // monotonic, captured at snapshot time
    StreamBuffer data;
    uint32_t checksum;           // CRC32C of data
};

struct EventRecord {
    std::string persistence_id;
    uint64_t sequence;
    uint32_t schema_version;
    uint32_t serializer_id;
    uint64_t timestamp_ms;
    StreamBuffer event_data;
};
```

#### Initial Implementations

| Store | Location | Purpose |
|-------|----------|---------|
| `InMemoryStateStore` | `src/actor/durable/in_memory_state_store.cpp` | Tests and single-node use. `std::unordered_map`-backed. |
| `FileStateStore` | `src/actor/durable/file_state_store.cpp` | Local durability. One file per actor under a configurable directory, atomic rename on write, CRC32C integrity checks. |

Future adapters: RocksDB, PostgreSQL, object storage — implement `DurableStateStore`
without changing actor code.

#### Schema Versioning

`SnapshotRecord` and `EventRecord` carry `schema_version` and `serializer_id`
fields. During recovery, if the stored schema version doesn't match the actor's
current version, `IDurableActor::migrate_snapshot()` is called with the stored
data and its schema version. Actors with multiple schema versions override this
to provide upgrade chains (e.g., v1→v2→v3). If no migration exists for a given
version gap, recovery fails with `FailureReason::SchemaVersionMismatch`.

#### Sequence Numbering

The `DurableStateStore` implementation owns the sequence counter. On
`write_snapshot()`, the store assigns the next monotonically increasing sequence
number for that `persistence_id`. The returned `SnapshotRecord` includes the
assigned sequence. During recovery, `load_events_after(snapshot_sequence)` replays
only events with sequence numbers greater than the restored snapshot.

#### Store Ownership

`DurableStateStore` is owned by `ActorSystem`. One store instance serves all
durable actors on the node. The store type is selected via TOML config or
programmatic `ActorSystem` construction.

### 3.8 Failure Reasons

New values in `FailureReason` (extending `include/hpactor/types/failure_reason.hpp`):

| Enum value | Code | Retryable | Meaning |
|------------|------|-----------|---------|
| `PassivationDrainTimeout` | 41 | true | Drain did not complete within deadline |
| `PassivationSnapshotFailed` | 42 | true | Durable store write failed |
| `ReactivationFailed` | 43 | true | Restore from durable store failed |
| `PassivationQueueFull` | 44 | true | Reactivation buffer exhausted |
| `SchemaVersionMismatch` | 45 | false | Stored schema version has no migration path |

### 3.9 Observability

#### Metrics (Prometheus)

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `hpactor_actor_passivation_total` | Counter | `trigger=[idle,self,memory_pressure,cli]` | Total passivation transitions |
| `hpactor_actor_reactivation_total` | Counter | `outcome=[success,failed]` | Total reactivation transitions |
| `hpactor_actor_passivated_count` | Gauge | — | Current number of passivated actors |
| `hpactor_passivation_drain_duration_ms` | Histogram | — | Time spent draining mailbox |
| `hpactor_passivation_snapshot_duration_ms` | Histogram | — | Time spent writing snapshot |
| `hpactor_reactivation_restore_duration_ms` | Histogram | — | Time spent restoring state |
| `hpactor_passivation_queue_depth` | Gauge | — | Current depth of reactivation buffers |

#### Metric Events (MpscRingBuffer)

New `MetricEventType` values:

| Enum value | Code | Emitted when |
|------------|------|-------------|
| `kPassivationStarted` | 27 | Actor begins passivation transition |
| `kPassivationCompleted` | 28 | Actor reaches `kPassivated` |
| `kReactivationStarted` | 29 | Message triggers reactivation |
| `kReactivationCompleted` | 30 | Actor reaches `kActive` after reactivation |
| `kReactivationFailed` | 31 | Reactivation fails, actor enters `kFailed` |

#### DLQ Integration

Messages that cannot be delivered due to passivation failures:

| Scenario | `DeadLetterReason` |
|----------|-------------------|
| Recovery from durable store fails | `ReactivationFailed` (new) |
| Reactivation buffer overflow | `PassivationQueueFull` (new) |
| Drain timeout during passivation | `PassivationDrainTimeout` (new, message dead-lettered if not processed) |

#### Structured Logging

Log events at key transitions:
- `passivation_started` / `passivation_completed` / `passivation_failed`
- `reactivation_started` / `reactivation_completed` / `reactivation_failed`
- `snapshot_write` / `snapshot_read` (with duration and byte count)
- `schema_migration` (from_version → to_version)

#### CLI Commands

| Command | Output |
|---------|--------|
| `/actor passivate <id>` | Triggers immediate passivation |
| `/actor reactivate <id>` | Triggers immediate reactivation of a passivated actor |
| `/actor list passivated` | Table: ActorId, persistence_id, passivated_at, idle duration, schema_version |
| `/actor show <id>` | Extended: shows passivation status, `passivated_at`, `snapshot_sequence`, trigger source |
| `/durable store stats` | DurableStateStore statistics: total snapshots, total events, bytes stored, store type |

### 3.10 Configuration

#### TOML (`[system.passivation]`)

```toml
[system.passivation]
enabled = true
default_idle_timeout_ms = 600000       # 10 min; 0 = disabled
memory_pressure_enabled = true
memory_pressure_high_threshold_pct = 85
memory_pressure_poll_interval_ms = 5000
max_reactivation_queue_depth = 64
drain_timeout_ms = 30000               # Max time for drain phase

[system.passivation.store]
type = "file"                           # "memory" | "file"
directory = "/var/lib/hpactor/durable"  # For FileStateStore
```

Per-actor override in TOML actor definitions:

```toml
[[actor]]
name = "order-cache"
behavior = "OrderCache"
passivation.idle_timeout_ms = 300000   # 5 min
passivation.durable = true
passivation.allow_memory_pressure = true
```

#### Programmatic API

```cpp
PassivationConfig cfg;
cfg.idle_timeout = std::chrono::minutes(5);
cfg.durable = true;
cfg.allow_memory_pressure = true;
auto aid = system.spawn<OrderCache>(cfg);
```

#### CMake

| Option | Default | Purpose |
|--------|---------|---------|
| `ENABLE_ACTOR_PASSIVATION` | ON | Gates the passivation subsystem at compile time via `HPACTOR_ENABLE_PASSIVATION` |

### 3.11 Integration Points

| Subsystem | Integration |
|-----------|------------|
| **Supervision** | If reactivation fails, the supervisor receives `on_child_failure()` with `FailureReason::ReactivationFailed`. Standard restart or quarantine policy applies. |
| **Shutdown** | During `ActorSystem::shutdown()`, passivated actors transition `Passivated → Stopped` without reactivation. Durable state is cleaned up. |
| **Link/Monitor** | Passivation does NOT generate a `DownMsg`. Linked actors are not notified — the passivated actor is still logically alive. Termination during `kPassivated` (shutdown or explicit kill) does generate `DownMsg`. |
| **CLI InspectState** | `InspectStateRequest` sent to a passivated actor returns the `PassivationRecord` metadata. |
| **MetricsActor** | Passivation metrics are emitted via the existing `MpscRingBuffer` → `MetricsActor` → `/metrics` path. |
| **Fault Injection** | See §3.12 for full fault-point design. |

### 3.12 Deterministic Fault Injection for Passivation

Passivation failure paths — drain timeout, snapshot write failure, reactivation
restore failure, buffer overflow — are inherently timing- and I/O-dependent.
Without deterministic control, tests for these paths would require real disk
errors, clock manipulation, or thread-level races. The project's existing
`FaultController` / `FAULT_INJECT` system (§2.6 of CLAUDE_MEMORY.md: 80 sites,
14 domains) provides the mechanism to make every passivation failure
deterministically testable.

#### 3.12.1 Fault Domain

A new fault domain `Passivation` (domain index 14) is added to `FaultDomain`.
All passivation fault points increment this domain's tick counter.

#### 3.12.2 Fault Point Catalog

| Fault Path | Action | Effect |
|-----------|--------|--------|
| `hpactor.passivation.idle.timer_fire` | `Fail` | Idle timer fires immediately regardless of elapsed time |
| `hpactor.passivation.transition.fail` | `Fail` | `LifecycleActor::transition(kPassivating)` CAS fails, passivation aborted |
| `hpactor.passivation.drain.timeout` | `Fail` | Drain deadline expires before mailbox is empty |
| `hpactor.passivation.drain.stall` | `Delay` | Drain blocks for N ticks, simulating a slow message handler |
| `hpactor.passivation.snapshot.write_fail` | `Fail` | `DurableStateStore::write_snapshot()` returns error |
| `hpactor.passivation.snapshot.corrupt` | `Corrupt` | Snapshot data is bit-flipped before write; integrity check catches it on read |
| `hpactor.passivation.reactivation.restore_fail` | `Fail` | `DurableStateStore::load_latest_snapshot()` returns error |
| `hpactor.passivation.reactivation.deserialize_fail` | `Fail` | `IDurableActor::restore_snapshot()` or `Hibernatable::deserialize_from()` returns error |
| `hpactor.passivation.reactivation.buffer_full` | `Fail` | Reactivation queue rejects new messages (emulates `PassivationQueueFull`) |
| `hpactor.passivation.reactivation.migrate_fail` | `Fail` | `IDurableActor::migrate_snapshot()` returns error |
| `hpactor.passivation.memory_pressure.trigger` | `Fail` | `MemoryPressureMonitor` triggers a passivation cycle immediately |
| `hpactor.passivation.memory_pressure.lru_select` | `Drop` | Selected LRU actor is skipped (not passivated), tests cascading selection |

#### 3.12.3 Wiring Locations

Each fault point is wired via `FAULT_INJECT(path)` at the corresponding call
site:

```cpp
// In LifecycleActor::transition() — abort passivation
FAULT_INJECT("hpactor.passivation.transition.fail") {
    return false;
}

// In PassivationManager::drain_mailbox() — force drain timeout
FAULT_INJECT("hpactor.passivation.drain.timeout") {
    deadline = clock::now(); // expire immediately
}

// In PassivationManager::write_snapshot() — inject write failure
FAULT_INJECT("hpactor.passivation.snapshot.write_fail") {
    return error::make(FailureReason::PassivationSnapshotFailed);
}

// In DurableStateStore::load_latest_snapshot() — inject restore failure
FAULT_INJECT("hpactor.passivation.reactivation.restore_fail") {
    return error::make(FailureReason::ReactivationFailed);
}
```

All sites use `HPACTOR_UNLIKELY` via the existing `FAULT_INJECT` macro. When
`ENABLE_FAULT_INJECTION=OFF`, the branch is eliminated at compile time — zero
runtime overhead.

#### 3.12.4 Test Usage Patterns

**Idle passivation trigger test:**
```cpp
// Inject idle timer fire — actor passivates without waiting for real timeout
fault.schedule("hpactor.passivation.idle.timer_fire", FaultAction::Fail, tick=1);
// Spawn actor, send one message (processed), then no more messages
// After the message is processed, idle timer "fires" immediately
// Assert: actor state == kPassivated
```

**Snapshot failure → supervisor restart:**
```cpp
// Actor processes messages, enters passivation, drain completes,
// but snapshot write fails
fault.schedule("hpactor.passivation.snapshot.write_fail", FaultAction::Fail, tick=3);
// Spawn actor with supervisor, let it idle-passivate
// Assert: actor state == kFailed (passivation snapshot failed)
// Assert: supervisor restarts actor
```

**Reactivation failure → DLQ:**
```cpp
// Actor passivates successfully, then a message triggers reactivation,
// but restore fails
fault.schedule("hpactor.passivation.reactivation.restore_fail", FaultAction::Fail, tick=2);
// Passivate actor, then send a wakeup message
// Assert: actor state == kFailed
// Assert: wakeup message lands in DLQ with DeadLetterReason::ReactivationFailed
```

**Reactivation buffer full:**
```cpp
// Reactivation is slow (stalled), senders flood the buffer
fault.schedule("hpactor.passivation.reactivation.buffer_full", FaultAction::Fail, tick=5);
// Passivate actor, send N messages (N > reactivation queue capacity)
// Assert: messages beyond capacity get EnqueueResult::Rejected with
//         FailureReason::PassivationQueueFull
```

**Memory pressure cascading:**
```cpp
// First LRU actor is skipped, second is selected
fault.schedule("hpactor.passivation.memory_pressure.trigger", FaultAction::Fail, tick=1);
fault.schedule("hpactor.passivation.memory_pressure.lru_select", FaultAction::Drop, tick=2);
// Register 3 idle actors, trigger memory pressure
// Assert: actor 1 skipped, actor 2 passivated, actor 3 left alone
```

#### 3.12.5 Seed Replay

All fault schedules are derived from a seed value. The same seed produces the
same sequence of fault injections across test runs, machines, and platforms.
This means passivation failure scenarios are fully reproducible — a test that
fails in CI with seed `0xDEAD` will fail identically when replayed locally.

## 4. New and Modified Files

### New Headers

| File | Purpose |
|------|---------|
| `include/hpactor/actor/passivation_config.hpp` | `PassivationConfig` struct, `PassivationRecord` struct |
| `include/hpactor/actor/durable_actor.hpp` | `IDurableActor` interface |
| `include/hpactor/actor/durable_state_store.hpp` | `DurableStateStore` interface, `SnapshotRecord`, `EventRecord` |
| `include/hpactor/actor/actor_route.hpp` | `IActorRoute`, `LocalActiveRoute`, `LocalPassivatedRoute` |
| `include/hpactor/actor/memory_pressure_monitor.hpp` | `MemoryPressureMonitor` class |

### New Source Files

| File | Purpose |
|------|---------|
| `src/actor/lifecycle_actor.cpp` (modified) | Add `kPassivating`/`kPassivated` transition handling |
| `src/actor/passivation_manager.cpp` | Passivation protocol orchestration |
| `src/actor/memory_pressure_monitor.cpp` | Memory pressure polling and LRU selection |
| `src/actor/actor_route.cpp` | Route implementations |
| `src/actor/durable/in_memory_state_store.cpp` | In-memory store for tests |
| `src/actor/durable/file_state_store.cpp` | File-backed store |
| `src/config/parsers/passivation_config_parser.cpp` | Self-registering TOML parser for `[system.passivation]` |
| `src/fault/passivation_fault_points.cpp` | Passivation fault-point registration (12 fault points)

### Modified Files

| File | Change |
|------|--------|
| `include/hpactor/actor/lifecycle_state.hpp` | Add `kPassivating = 8`, `kPassivated = 9`; extend `kStateMachine` table |
| `include/hpactor/actor/lifecycle_actor.hpp` | Add `on_passivating()`, `on_passivated()` hooks |
| `include/hpactor/types/failure_reason.hpp` | Add 5 new `FailureReason` values (41-45) |
| `include/hpactor/core/actor_system.hpp` | Add `PassivationManager` and `MemoryPressureMonitor` ownership, `DurableStateStore` accessor |
| `include/hpactor/actor/actor_context.hpp` | Add `passivate()` method |

### New Test Files

| File | Tier | Tests |
|------|------|-------|
| `tests/unit/actor/test_passivation_config.cpp` | Unit | Config parsing, defaults, per-actor overrides |
| `tests/unit/actor/test_passivation_lifecycle.cpp` | Unit | State transitions, illegal transitions rejected |
| `tests/unit/actor/test_passivation_triggers.cpp` | Unit | Idle timer, self-request, CLI command, memory pressure |
| `tests/unit/actor/test_passivation_drain.cpp` | Unit | Drain before snapshot, drain timeout → Failed |
| `tests/unit/actor/test_actor_route.cpp` | Unit | Route stub buffers messages, triggers reactivation, queue-full |
| `tests/unit/actor/test_durable_state_store.cpp` | Unit | InMemory + File: write/load/delete snapshot, append/load events |
| `tests/unit/actor/test_schema_migration.cpp` | Unit | Version mismatch, migration, unmigratable → Failed |
| `tests/unit/actor/test_durable_actor.cpp` | Unit | IDurableActor: snapshot→restore roundtrip, event application |
| `tests/integration/actor/test_passivation_reactivation.cpp` | Integration | Full cycle: spawn → idle → passivate → send → reactivate → deliver |
| `tests/integration/actor/test_passivation_durable.cpp` | Integration | Full cycle with FileStateStore |
| `tests/integration/actor/test_passivation_memory_pressure.cpp` | Integration | Memory pressure monitor selects LRU actors |
| `tests/integration/actor/test_passivation_supervision.cpp` | Integration | Failed reactivation → supervisor restart/quarantine |
| `tests/integration/actor/test_passivation_shutdown.cpp` | Integration | Passivated actor shutdown → Stopped, durable state cleaned up |
| `tests/system/test_passivation_end_to_end.cpp` | System | Multi-actor scenario: idle timeouts, message-triggered reactivation, CLI |
| `tests/unit/fault/test_passivation_fault_points.cpp` | Unit | All 12 fault points register, fire, and produce correct failure outcomes |
| `tests/unit/fault/test_passivation_fault_seed_replay.cpp` | Unit | Same seed → same passivation failure sequence across 10 runs |
| `tests/integration/actor/test_passivation_fault_injection.cpp` | Integration | Fault-injected snapshot failure, reactivation failure, buffer-full scenarios |

## 5. Acceptance Criteria

- [ ] Lifecycle state machine includes `kPassivating` and `kPassivated` with validated transitions.
- [ ] Idle timeout triggers passivation after configurable inactivity period.
- [ ] `ActorContext::passivate()` allows self-requested passivation.
- [ ] Memory pressure monitor selects and passivates LRU actors above the high watermark.
- [ ] CLI `/actor passivate <id>` and `/actor reactivate <id>` work.
- [ ] Drain phase processes or times out queued messages before snapshot.
- [ ] `IDurableActor` implementations can snapshot and restore state via `DurableStateStore`.
- [ ] `Hibernatable` actors can passivate to `HibernationRegistry` without a durable store.
- [ ] `LocalPassivatedRoute` buffers messages and triggers lazy reactivation.
- [ ] Reactivation restores state before delivering any buffered messages.
- [ ] Failed reactivation dead-letters buffered messages and notifies the supervisor.
- [ ] Passivated actors survive system shutdown cleanly (state deleted, no leaks).
- [ ] Schema version mismatch during recovery produces a clear diagnostic.
- [ ] Passivation metrics, log events, and CLI introspection are wired.
- [ ] All 12 passivation fault points register and fire deterministically via `FaultController`.
- [ ] Fault-injected passivation failures (snapshot fail, drain timeout, reactivation fail, buffer full) produce correct lifecycle transitions, DLQ records, and supervision outcomes.
- [ ] Seed replay: same fault schedule seed produces identical passivation failure sequence across runs.
- [ ] Non-passivated actors incur zero overhead when `ENABLE_ACTOR_PASSIVATION=ON`.
- [ ] All new files carry Apache 2.0 license headers.

## 6. Out of Scope (Explicit Non-Goals)

- **Transparent persistence for arbitrary C++ object graphs.** Actors must
  explicitly implement `IDurableActor` or `Hibernatable`.
- **Distributed transactions across multiple actors during passivation.**
  Each actor passivates independently.
- **Shard-owner route implementation (`ShardOwnedRoute`).** The `IActorRoute`
  interface is designed to accommodate it, but implementation waits for
  sharding (CLS-003).
- **RocksDB, PostgreSQL, or object storage backends.** Only `InMemoryStateStore`
  and `FileStateStore` are in scope.
- **Event-sourced actor runtime support beyond the `apply_event()` interface.**
  The `IDurableActor` interface supports event application, but event journaling
  APIs (`persist_event()`, `persist_event_and_apply()`) are deferred to a
  follow-on design that builds on this storage layer.
