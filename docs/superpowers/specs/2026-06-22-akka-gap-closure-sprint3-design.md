# Akka Gap Closure Sprint 3 — Durability + Operations Design

## 1. Executive Summary

Sprint 3 of the Akka Typed Actors gap closure (issue #329) closes the three
highest-impact remaining P1 gaps:

| # | Subsystem | Issue Status | Design Doc |
|---|-----------|-------------|------------|
| 1 | **MSG-003** Reliable Messaging (ACK/NACK/retry) | Not started | `docs/architecture/production/reliable-messaging-design.md` |
| 2 | **CLU-003** Cluster Singleton Actor Integration | Types done, actor wiring missing | Feature gap backlog CLU-003 card |
| 3 | **DUR-001/002** Durable Actor Runtime | Store infrastructure exists; no behaviors or lifecycle | `docs/architecture/production/durable-actor-state-design.md` |

Sprint 1 (#344) closed 10 developer-ergonomics gaps. Sprint 2 (#347) implemented
CLU-001 (Cluster Failure Model), CLU-002 (Cluster Sharding), and the
infrastructure types for CLU-003 (SingletonManagerCore, OldestNodeElection).
Sprint 3 completes the cluster plane with singleton actor wiring, adds reliable
messaging, and delivers the first durable actor runtime.

These three subsystems are architecturally independent but integrate at key
touchpoints: `SingletonManagerActor` spawns `ShardCoordinatorActor` as its first
singleton consumer, which drives `ShardHandoff` → `PassivationManager`
passivation of durable actors during ownership transfer.

## 2. MSG-003: Reliable Messaging (ACK/NACK/Retry)

### 2.1 Core Concept

Opt-in at-least-once delivery for user messages. Built on existing `DeliveryMode`,
`FailureEnvelope`, `DedupCache`, and `MultiLaneQueue`. ACK is sent after
mailbox admission, not after handler completion — it proves the message entered
the receiver's runtime without tying sender progress to handler latency.

### 2.2 Components

| File | Purpose |
|------|---------|
| `include/hpactor/mailbox/outbound_tracker.hpp` | `OutboundTracker` — bounded per-destination pending map |
| `include/hpactor/mailbox/reliable_retry_policy.hpp` | `ReliableRetryPolicy` struct |
| `include/hpactor/net/reliable_ack.hpp` | `AckStatus`, `AckPayload`, frame flag constants |
| `include/hpactor/msg/durable_delivery_store.hpp` | Existing interface — `InMemoryDeliveryStore` and `FileDeliveryStore` adapters added |
| `src/mailbox/outbound_tracker.cpp` | Tracker implementation |
| `src/mailbox/in_memory_delivery_store.cpp` | In-memory adapter for tests and non-durable mode |
| `src/mailbox/file_delivery_store.cpp` | File adapter for restart recovery |
| `src/net/reliable_ack.cpp` | ACK/NACK binary wire format |
| `src/config/parsers/reliable_messaging_config_parser.cpp` | TOML `[system.reliable]` parser |

### 2.3 Key Types

**AckStatus and AckPayload — compact binary ACK/NACK (no protobuf):**

```cpp
enum class AckStatus : uint8_t {
    Accepted = 0,    // Message admitted to receiver mailbox
    Rejected = 1,    // Receiver rejected (full, draining, policy)
    Duplicate = 2,   // Already seen, suppressed
};

struct AckPayload {
    MessageId message_id;
    AckStatus status;
    Duration retry_after;  // 0 if not applicable
};
// Wire format: 14 bytes (message_id 8B + status 1B + padding 1B + retry_after 4B)
```

**OutboundTrackerEntry:**

```cpp
struct OutboundTrackerEntry {
    MessageId message_id;
    ActorAddr target;
    StreamBuffer payload;
    uint32_t retry_count = 0;
    MonotonicClock::time_point next_retry_at;
    MonotonicClock::time_point deadline;
};
```

**OutboundTracker:**

```cpp
class OutboundTracker {
public:
    static constexpr size_t kMaxPendingPerDestination = 1024;

    auto track(MessageId msg_id, ActorAddr target, StreamBuffer payload,
               MonotonicClock::time_point deadline) -> bool;
    auto on_ack(MessageId msg_id) -> void;
    auto on_nack(MessageId msg_id, Duration retry_after) -> void;
    auto tick(MonotonicClock::time_point now) -> void;
    auto fail_pending_for_node(const std::string& node_id) -> void;
    auto pending_count() const -> size_t;
};
```

**ReliableRetryPolicy:**

```cpp
struct ReliableRetryPolicy {
    uint32_t max_retries = 3;
    Duration initial_backoff = std::chrono::milliseconds(100);
    Duration max_backoff = std::chrono::seconds(10);
    double backoff_multiplier = 2.0;
};
```

### 2.4 Wire Protocol

Two new frame flag bits on the existing Frame header — no new protobuf types:

- `AckRequested (0x10)` — sender requests ACK. Set when `DeliveryMode::AtLeastOnce`.
- `AckResponse (0x20)` — this frame is an ACK or NACK.

ACK/NACK frames carry `AckPayload` in a compact 14-byte binary payload following
the frame header. Best-effort messages (no `AckRequested` flag) skip all ACK/NACK
processing.

### 2.5 Delivery Lifecycle

```
1. Sender creates message with stable MessageId, DeliveryMode::AtLeastOnce.
2. Reliable manager stores message in OutboundTracker.
3. Transport sends frame with AckRequested flag.
4. Receiver EventBasedActor::receive() checks DedupCache.
5.   Duplicate → emit ACK(Duplicate), suppress redelivery.
6.   New → admit to mailbox → emit ACK(Accepted).
7.   Mailbox high watermark → emit NACK(Rejected, retry_after).
8.   Actor draining with DropUserMessages → emit NACK(Rejected).
9. Sender receives ACK → remove from OutboundTracker.
10. Sender receives NACK → reschedule with retry_after delay.
11. Timer fires → resend (up to max_retries).
12. Retry exhaustion → DLQ with FailureReason::RetryExhausted.
13. Sender receives ACK(Duplicate) → mark complete (message was delivered).
```

### 2.6 Receiver Auto-ACK Integration

In `EventBasedActor::receive()`, after the existing `DedupCache` check:

```
if delivery_mode.has(AckRequested):
    if dedup_check == Duplicate:
        emit ACK(Duplicate)
        return  // suppress redelivery
    if mailbox_depth > high_watermark:
        emit NACK(Rejected, retry_after=backoff_hint)
        return  // message not admitted
    if draining and policy == DropUserMessages:
        emit NACK(Rejected)
        return
    // Otherwise: admit message, emit ACK(Accepted) after admission
```

Best-effort messages (no `AckRequested`) skip all ACK logic — zero overhead.

### 2.7 DurableDeliveryStore Adapters

Using the existing `DurableDeliveryStore` interface
(`include/hpactor/msg/durable_delivery_store.hpp`):

- **`InMemoryDeliveryStore`** — `std::unordered_map<MessageId, PendingSend>`
  backend. For tests and non-durable reliable mode. No restart recovery.
- **`FileDeliveryStore`** — One file per node ID in `root_dir/`. Atomic rename on
  write (temp → final). CRC32C checksums (reuses pattern from `FileStateStore`).
  Survives process restart.

On `ActorSystem` startup with durable reliable messaging enabled: load pending
outbox from store, re-register entries in `OutboundTracker`, resume retry timers.

### 2.8 TOML Configuration

```toml
[system.reliable]
enabled = true                # default false — opt-in
max_retries = 3
initial_backoff_ms = 100
max_backoff_ms = 10000
backoff_multiplier = 2.0
max_pending_per_destination = 1024
durable = true                # use FileDeliveryStore instead of InMemoryDeliveryStore
file_root_dir = "/var/lib/hpactor/delivery"
```

### 2.9 Integration Points

- **DLQ**: retry exhaustion creates `DeadLetterRecord` with
  `FailureReason::RetryExhausted` and full `FailureEnvelope` correlation metadata.
- **CLU-001**: `ClusterFailureModel` node-down → `OutboundTracker::fail_pending_for_node(node_id)`
  → all pending sends to that node DLQ'd with `FailureReason::NodeUnavailable`.
- **Metrics** (via existing `MpscRingBuffer`): `hpactor_reliable_outbox_pending`,
  `hpactor_reliable_acks_total`, `hpactor_reliable_nacks_total`,
  `hpactor_reliable_retries_total`, `hpactor_reliable_duplicates_total`,
  `hpactor_reliable_retry_exhausted_total`.
- **CLI**: `/reliable pending [node_id]`, `/reliable stats`, `/reliable drop <message_id>`.

### 2.10 Modified Existing Files

| File | Change |
|------|--------|
| `include/hpactor/net/frame.hpp` | Add `AckRequested (0x10)`, `AckResponse (0x20)` flags |
| `src/actor/event_based_actor.cpp` | Auto-ACK/NACK after dedup check + mailbox admission |
| `include/hpactor/msg/failure_reason.hpp` | Add `RetryExhausted` reason (transport range) |
| `src/msg/failure_reason.cpp` | `to_string()` and `retryable()` for `RetryExhausted` |
| `include/hpactor/core/actor_system.hpp` | Own `OutboundTracker`, wire on cluster init if enabled |

### 2.11 New Tests

| Test File | Scope | Approx. Cases |
|-----------|-------|---------------|
| `tests/unit/mailbox/test_outbound_tracker.cpp` | track, ack, nack, expiry, tick, capacity, node-down | 12 |
| `tests/unit/mailbox/test_reliable_retry_policy.cpp` | Backoff math, max retries, edge cases | 5 |
| `tests/unit/net/test_reliable_ack.cpp` | Encode/decode roundtrip, all AckStatus values | 5 |
| `tests/unit/mailbox/test_delivery_store.cpp` | InMemory + File stores: put, mark, load, seen | 8 |
| `tests/integration/mailbox/test_reliable_messaging.cpp` | End-to-end ACK/NACK, dup suppression, retry exhaustion, DLQ | 8 |

### 2.12 Acceptance Criteria

- `AtLeastOnce` delivery mode triggers `OutboundTracker` registration.
- ACK removes pending entry; NACK reschedules with `retry_after` delay.
- Retry with exponential backoff stops at `max_retries`.
- Retry exhaustion creates DLQ record with `FailureReason::RetryExhausted`
  and `FailureEnvelope`.
- Duplicate `message_id` is suppressed at receiver, ACK(Duplicate) sent anyway.
- Best-effort messages (no `AckRequested` flag) are completely unaffected.
- Bounded capacity: `max_pending_per_destination` enforced, overflow returns
  `false` from `track()`.
- `FileDeliveryStore` survives restart: `load_pending_outbox()` returns
  unacknowledged sends.
- Node-down (CLU-001) triggers `fail_pending_for_node()`, DLQ's all pending
  sends to that node.
- All tests deterministic — controlled `tick()`, `SchedulerTestDriver`, no
  real timers or threads.

---

## 3. CLU-003: Cluster Singleton Actor Integration

### 3.1 Core Concept

Wraps the existing `SingletonManagerCore` and `ShardCoordinatorCore` (standalone,
thread-safe classes from Sprint 2) into `EventBasedActor` subclasses so they
participate in the actor lifecycle, react to cluster membership events, and
integrate with `ActorSystem` spawn/stop.

### 3.2 Components

| File | Purpose |
|------|---------|
| `include/hpactor/cluster/singleton/singleton_manager_actor.hpp` | `SingletonManagerActor` — EventBasedActor wrapping `SingletonManagerCore` |
| `include/hpactor/cluster/sharding/shard_coordinator_actor.hpp` | `ShardCoordinatorActor` — EventBasedActor wrapping `ShardCoordinatorCore` |
| `src/cluster/singleton/singleton_manager_actor.cpp` | Actor implementation |
| `src/cluster/sharding/shard_coordinator_actor.cpp` | Actor implementation |

### 3.3 SingletonManagerActor

Wraps `SingletonManagerCore` in an `EventBasedActor`:

```
TypeTags handled:
  RegisterSingleton  — core_.register_singleton(id)
  NodeStateChange    — core_.on_node_state_change(alive_nodes)
                       → run elections → activate/deactivate local singletons
  BeginDrain         — core_.begin_drain(name)
  CompleteDrain      — core_.complete_drain(name)
```

**Election → Activation flow:**

1. `ClusterFailureModel` invokes registered callback with new `alive_nodes` list.
2. Callback sends `NodeStateChange` system message to `SingletonManagerActor`.
3. Actor calls `core_.on_node_state_change(alive_nodes)`.
4. For each registered singleton, `core_` runs `ISingletonElection::elect()`.
5. If this node wins: set `SingletonState::Activating` → spawn singleton actor
   locally → set `SingletonState::Active` → bump fencing token.
6. If this node loses: if previously active → set `SingletonState::Draining` →
   drain in-flight messages → stop local singleton → set `SingletonState::Standby`.

**Fencing token propagation:** Every singleton action message carries
`(singleton_name, fencing_token)` in the message header. If a message arrives
with a stale token, it's rejected with `FailureReason::FencingTokenStale`. This
prevents split-brain writes from a deposed owner.

### 3.4 ShardCoordinatorActor

Wraps `ShardCoordinatorCore` in an `EventBasedActor`. Registered as the first
cluster singleton via `SingletonManagerActor`.

```
TypeTags handled:
  RegisterShardActor   — core_.register_actor(logical_id, owner_node)
  UnregisterShardActor — core_.unregister_actor(logical_id)
  RebalanceRequest     — core_.rebalance(alive_nodes)
  GetShardOwner        — reply with owner node for shard_id
  HandoffComplete      — core shard handoff FSM progress
```

**Singleton lifecycle integration:**

- On `SingletonManagerActor` activation → spawn `ShardCoordinatorActor`.
- On `SingletonManagerActor` drain → `ShardCoordinatorActor` snapshots shard
  table to durable store, drains in-flight registrations, stops.
- On `SingletonManagerActor` reactivation (failover) → new node spawns
  `ShardCoordinatorActor`, loads shard table from durable store, resumes.

### 3.5 ActorSystem Integration

```cpp
// In ActorSystem initialization, when cluster mode is enabled:
if (cluster_enabled_) {
    // 1. Create ShardCoordinatorActor config (not yet spawned — singleton
    //    manages lifecycle)
    auto sc_config = SpawnConfig{...};

    // 2. Create SingletonManagerActor with election strategy
    auto election = std::make_unique<OldestNodeElection>();
    auto singleton_mgr = std::make_unique<SingletonManagerActor>(
        node_id_, std::move(election));

    // 3. Register shard-coordinator as the first managed singleton
    singleton_mgr->register_singleton(
        SingletonIdentity{"shard-coordinator", 0}, sc_config);

    // 4. Spawn SingletonManagerActor as a system actor (always running)
    spawn_system_actor(std::move(singleton_mgr));

    // 5. Wire ClusterFailureModel → SingletonManagerActor callback
    failure_model_->register_observer([this](const auto& alive_nodes) {
        send(singleton_mgr_addr_, NodeStateChange{alive_nodes});
    });
}
```

### 3.6 Modified Existing Files

| File | Change |
|------|--------|
| `include/hpactor/core/actor_system.hpp` | Own `SingletonManagerActor` ref + `ShardCoordinatorActor` config |
| `src/actor/actor_system.cpp` | Initialization code for singleton subsystem |
| `include/hpactor/cluster/cluster_failure_model.hpp` | Add observer callback registration |
| `include/hpactor/msg/type_tag.hpp` | New TypeTags for singleton/shard coordinator messages |
| `include/hpactor/cluster/singleton/singleton_manager.hpp` | `SingletonManagerCore` gains `get_registered()` |

### 3.7 New Tests

| Test File | Scope | Approx. Cases |
|-----------|-------|---------------|
| `tests/unit/cluster/singleton/test_singleton_manager_actor.cpp` | BehaviorTestKit: election → activation, standby, drain, fencing | 10 |
| `tests/unit/cluster/sharding/test_shard_coordinator_actor.cpp` | BehaviorTestKit: register, unregister, rebalance, get owner | 8 |
| `tests/integration/cluster/singleton/test_singleton_fencing.cpp` | Two ActorSystems, one loses connection → survivor activates | 5 |
| `tests/integration/cluster/singleton/test_shard_coordinator_singleton.cpp` | ShardCoordinatorActor failover, shard table recovery | 4 |

### 3.8 Acceptance Criteria

- At most one active singleton owner exists per singleton identity across
  the cluster.
- Singleton failover occurs when owner node transitions to Down (via CLU-001
  callback).
- Stale fencing tokens are rejected with `FailureReason::FencingTokenStale`.
- `ShardCoordinatorActor` is the first working consumer, running as a cluster
  singleton.
- ActorSystem spawns `SingletonManagerActor` automatically when cluster mode
  is enabled.
- CLI: `/cluster singletons`, `/cluster singleton <name> show`,
  `/cluster singleton <name> force-election`.
- All tests deterministic — `BehaviorTestKit` for unit, `SchedulerTestDriver`
  for integration.

---

## 4. DUR-001/002: Durable Actor Runtime

### 4.1 Core Concept

Provides actor-level behaviors and lifecycle management for durable actors.
Leverages the existing `DurableStateStore`, `FileStateStore`, `InMemoryStateStore`,
`SnapshotRecord`, and `EventRecord` infrastructure. Adds `DurableBehavior<State>`
(snapshot mode), `EventSourcedBehavior<State, Event>` (event-sourced mode),
recovery hooks in the spawn pipeline, and a `PassivationManager` system actor.

### 4.2 Components

| File | Purpose |
|------|---------|
| `include/hpactor/actor/durable/durable_behavior.hpp` | `DurableBehavior<State>` — snapshot-mode behavior template |
| `include/hpactor/actor/durable/event_sourced_behavior.hpp` | `EventSourcedBehavior<State, Event>` — event-sourced behavior template |
| `include/hpactor/actor/durable/passivation_manager.hpp` | `PassivationManager` — system actor managing passivation lifecycle |
| `include/hpactor/actor/durable/passivation_config.hpp` | `PassivationConfig` — per-actor passivation policy |
| `include/hpactor/actor/durable/recovery_policy.hpp` | `RecoveryPolicy` enum + recovery outcome |
| `src/actor/durable/passivation_manager.cpp` | Manager implementation |
| `src/config/parsers/durable_config_parser.cpp` | TOML `[system.durable]` parser |

### 4.3 DurableBehavior<State>

Template for snapshot-mode durable actors:

```cpp
template <typename State>
class DurableBehavior {
public:
    DurableBehavior(std::string persistence_id, DurableStateStore& store,
                    State initial);

    // Called by spawn pipeline before first message is delivered
    result<void> recover();

    // Called by PassivationManager before passivation
    result<void> snapshot();

    // Current in-memory state
    State& state();
    const State& state() const;
    const std::string& persistence_id() const;
    bool is_recovered() const;

private:
    std::string persistence_id_;
    DurableStateStore& store_;
    State state_;
    uint64_t last_snapshot_sequence_ = 0;
    bool recovered_ = false;
};
```

**User specializes** these template methods for serialization:
```cpp
result<StreamBuffer> serialize_state(const State& s);     // REQUIRED
result<State> deserialize_state(const StreamBuffer& data); // REQUIRED
```

### 4.4 EventSourcedBehavior<State, Event>

Template for event-sourced durable actors:

```cpp
template <typename State, typename Event>
class EventSourcedBehavior {
public:
    EventSourcedBehavior(std::string persistence_id, DurableStateStore& store,
                         State initial);

    // Recovery: load latest snapshot → replay events after snapshot sequence
    result<void> recover();

    // Persist event, then apply to state. Atomic: event persisted before
    // state mutation. Returns error if persistence fails.
    result<void> persist_event(const Event& event);
    result<void> persist_event_and_apply(const Event& event);

    // Snapshot current state + event sequence for fast recovery
    result<void> snapshot();

    State& state();
    bool is_recovered() const;

private:
    std::string persistence_id_;
    DurableStateStore& store_;
    State state_;
    uint64_t last_snapshot_sequence_ = 0;
    uint64_t last_event_sequence_ = 0;
    bool recovered_ = false;
};
```

**User specializes** these template methods:
```cpp
result<void> apply_event_to_state(State& s, const Event& e);        // REQUIRED
result<StreamBuffer> serialize_state(const State& s);               // REQUIRED
result<State> deserialize_state(const StreamBuffer& data);          // REQUIRED
result<StreamBuffer> serialize_event(const Event& e);               // REQUIRED
result<Event> deserialize_event(const StreamBuffer& data);          // REQUIRED
```

### 4.5 Recovery Lifecycle (Spawn Pipeline Hook)

When `EventBasedActor` is spawned with a `DurableStateStore` reference and an
actor implementing `IDurableActor`:

```
1. ActorSystem spawn calls EventBasedActor::initialize().
2. initialize() detects durable config → calls recover():
   a. store.load_latest_snapshot(persistence_id)
      → if found, deserialize → restore state.
   b. store.load_events_after(persistence_id, snapshot_sequence)
      → if events exist, replay in order.
   c. Schema version mismatch
      → migrate_snapshot(from_version, data) or fail.
   d. Checksum verification failure
      → fail with SnapshotCorrupt.
3. Actor emits RecoveryComplete signal.
4. Actor marked recovered_ = true.
5. Actor begins accepting user messages from mailbox.
```

**RecoveryPolicy:**

| Policy | Behavior on recovery failure |
|--------|------------------------------|
| `FailActor` | Actor terminates; supervisor handles (default) |
| `QuarantineActor` | Actor exists but rejects all user messages; operator must inspect |
| `SkipCorruptEvent` | Skip the corrupt event, continue replay (tolerant mode only) |

### 4.6 PassivationManager

System actor managing passivation lifecycle for durable actors:

```
TypeTags handled:
  PassivateRequest    — Begin passivation for an actor
  PassivateComplete   — Actor finished snapshotting → release memory
  PassivationTimeout  — Force-complete if actor didn't respond in time
  IdleTimeoutCheck    — Periodic scan: passivate idle durable actors
  ReactivateRequest   — Spawn actor, trigger recovery, deliver pending message
```

**Passivation sequence:**

```
1. Trigger: idle timeout, explicit CLI command, or shard handoff (CLU-003).
2. PassivationManager sends PassivateRequest to actor.
3. Actor stops accepting new user messages (mailbox gate).
4. Actor drains in-flight messages (respects DrainPolicy).
5. Actor calls behavior.snapshot() → store.write_snapshot(...).
6. Actor replies PassivateComplete.
7. PassivationManager releases actor memory (ActorSystem::stop()).
8. Actor's logical route stays active through shard owner.
9. Next message to actor → ReactivateRequest → spawn → recover → deliver.
```

**PassivationConfig:**

```cpp
struct PassivationConfig {
    Duration idle_timeout = std::chrono::minutes(30);
    uint32_t schema_version = 1;
    RecoveryPolicy recovery_policy = RecoveryPolicy::FailActor;
    bool snapshot_on_passivate = true;
    bool snapshot_on_shutdown = true;
};
```

### 4.7 TOML Configuration

```toml
[system.durable]
enabled = true
store_type = "file"                    # "file" or "in_memory"
file_root_dir = "/var/lib/hpactor/state"
default_idle_timeout_seconds = 1800    # 30 min
default_schema_version = 1
```

Per-actor override:
```toml
[[actors]]
name = "order-processor"
behavior = "OrderProcessor"
durable = true
persistence_id = "order-processor-1"
idle_timeout_seconds = 3600
schema_version = 2
```

### 4.8 Integration with CLU-003 Shard Handoff

When `ShardCoordinatorActor` initiates handoff for a shard `Owned → Draining`:

1. `ShardCoordinatorActor` sends `PassivateRequest` to all durable actors
   in the shard.
2. `PassivationManager` snapshots each actor's state via `DurableStateStore`.
3. On `PassivateComplete`, `ShardCoordinatorActor` proceeds to `Transferring`.
4. Shard ownership transfers to new node.
5. New node spawns actors → recovery loads snapshots → replays events →
   actors become Active.
6. `ShardCoordinatorActor` publishes new shard table epoch.

### 4.9 Modified Existing Files

| File | Change |
|------|--------|
| `include/hpactor/core/actor_system.hpp` | Own `PassivationManager` + `DurableStateStore` |
| `src/actor/event_based_actor.cpp` | Recovery hook in spawn pipeline |
| `include/hpactor/actor/event_based_actor.hpp` | Recovery gate: reject user messages until `recovered_` |
| `include/hpactor/msg/type_tag.hpp` | New TypeTags for passivation and recovery |
| `include/hpactor/msg/failure_reason.hpp` | Add `SchemaVersionMismatch`, `SnapshotCorrupt`, `RecoveryFailed` |
| `include/hpactor/cluster/singleton/singleton_manager_actor.hpp` | Expose `PassivationManager` addr for shard handoff |

### 4.10 New Tests

| Test File | Scope | Approx. Cases |
|-----------|-------|---------------|
| `tests/unit/actor/durable/test_durable_behavior.cpp` | Snapshot → recover; modify → snapshot → recover; schema migration | 8 |
| `tests/unit/actor/durable/test_event_sourced_behavior.cpp` | Persist events → recover → replay order; hybrid mode | 8 |
| `tests/unit/actor/durable/test_passivation_manager.cpp` | Passivate → snapshot → release; idle timeout; force-complete | 10 |
| `tests/unit/actor/durable/test_recovery_policy.cpp` | FailActor, QuarantineActor, SkipCorruptEvent outcomes | 6 |
| `tests/integration/actor/test_durable_workflow.cpp` | Full passivation → reactivation; shard handoff integration | 8 |

### 4.11 Acceptance Criteria

- Durable actors opt in explicitly via `PassivationConfig` or template choice.
- Non-durable actors incur zero overhead — no recovery code runs for them.
- Snapshot recovery completes before first user message is delivered (recovery gate).
- Event replay preserves event order per actor (ascending sequence numbers).
- Schema version mismatch invokes `migrate_snapshot()` or fails with clear diagnostics.
- Checksum verification on snapshot load detects corruption.
- Passivation snapshots state, releases actor memory, keeps route active.
- Reactivation loads snapshot + replays events, actor becomes ready before
  new messages.
- `PassivationManager` periodic scan passivates idle actors after configurable timeout.
- `ShardCoordinatorActor` handoff triggers passivation for durable actors in
  the shard.
- All tests deterministic — `SchedulerTestDriver`, `InMemoryStateStore`,
  no real timers.

---

## 5. Cross-Cutting Concerns

### 5.1 FailureReason Additions

| Code | Name | Subsystem | Retryable | Description |
|------|------|-----------|-----------|-------------|
| TBD | `RetryExhausted` | MSG-003 | No | Reliable message retry limit reached; message DLQ'd |
| TBD | `FencingTokenStale` | CLU-003 | Yes | Singleton fencing token is stale; retry with new owner |
| TBD | `SchemaVersionMismatch` | DUR-001 | No | Snapshot schema version incompatible with current code |
| TBD | `SnapshotCorrupt` | DUR-001 | No | Snapshot checksum verification failed |
| TBD | `RecoveryFailed` | DUR-001 | No | Recovery of durable state failed (store error, corrupt data) |

### 5.2 TypeTag Additions

| Tag Range | Name | Subsystem |
|-----------|------|-----------|
| 0x76 | `SingletonElectionRequest` | CLU-003 |
| 0x77 | `SingletonFencingToken` | CLU-003 |
| 0x78 | `ShardCoordinatorCommand` | CLU-003 |
| 0x79 | `PassivateRequest` | DUR-001 |
| 0x7A | `PassivateComplete` | DUR-001 |
| 0x7B | `RecoveryComplete` | DUR-001 |

### 5.3 CLI Commands

| Command | Subsystem |
|---------|-----------|
| `/reliable pending [node_id]` | MSG-003 |
| `/reliable stats` | MSG-003 |
| `/reliable drop <message_id>` | MSG-003 |
| `/cluster singletons` | CLU-003 |
| `/cluster singleton <name> show` | CLU-003 |
| `/cluster singleton <name> force-election` | CLU-003 |
| `/durable actor <id> state` | DUR-001 |
| `/durable actor <id> recover` | DUR-001 |
| `/durable actor <id> passivate` | DUR-001 |
| `/durable store stats` | DUR-001 |

### 5.4 Observability Metrics

| Metric | Subsystem | Type |
|--------|-----------|------|
| `hpactor_reliable_outbox_pending` | MSG-003 | Gauge |
| `hpactor_reliable_acks_total` | MSG-003 | Counter |
| `hpactor_reliable_nacks_total` | MSG-003 | Counter |
| `hpactor_reliable_retries_total` | MSG-003 | Counter |
| `hpactor_reliable_duplicates_total` | MSG-003 | Counter |
| `hpactor_reliable_retry_exhausted_total` | MSG-003 | Counter |
| `hpactor_cluster_singleton_owner` | CLU-003 | Gauge (1=owner, 0=standby) |
| `hpactor_cluster_singleton_failover_total` | CLU-003 | Counter |
| `hpactor_cluster_singleton_fencing_rejects_total` | CLU-003 | Counter |
| `hpactor_durable_recovery_total` | DUR-001 | Counter |
| `hpactor_durable_recovery_duration_seconds` | DUR-001 | Histogram |
| `hpactor_durable_snapshot_write_total` | DUR-001 | Counter |
| `hpactor_durable_event_append_total` | DUR-001 | Counter |
| `hpactor_durable_store_errors_total` | DUR-001 | Counter |
| `hpactor_durable_passivations_total` | DUR-001 | Counter |
| `hpactor_durable_reactivations_total` | DUR-001 | Counter |

### 5.5 Deterministic Testing Contract

All tests follow `.claude/rules` testing constraints:

- **No real threads** in unit tests. Use `SchedulerTestDriver` with worker
  count = 0 where scheduler-dependent behavior needs observation, and
  `BehaviorTestKit` for synchronous behavior testing.
- **No sleep/wall-clock timing assumptions.** Use controlled `tick()` loops
  for retry timers, condition-based polling with generous timeouts (5s+)
  only in integration tests.
- **State machine tests** verify transitions programmatically — no reliance
  on thread interleaving.
- **Fencing tests** inject identity conflicts via direct `ClusterFailureModel`
  method calls, not by spawning real duplicate nodes.
- **Durable tests** use `InMemoryStateStore` — fast, inspectable, no filesystem
  dependencies.
- **Reliable messaging tests** use a controlled `tick()` loop, not real timers.
  Injected ACK/NACK responses, not real network.
- **Singleton actor tests** use `BehaviorTestKit` — synchronous, no ActorSystem
  needed.

### 5.6 Build & CMake

Minimal build changes — most new files go into existing library targets:

```cmake
# src/mailbox/CMakeLists.txt — add to existing hpactor_mailbox
# outbound_tracker.cpp, in_memory_delivery_store.cpp, file_delivery_store.cpp

# src/cluster/singleton/CMakeLists.txt — already part of hpactor_cluster
# singleton_manager_actor.cpp

# src/cluster/sharding/CMakeLists.txt — already part of hpactor_cluster
# shard_coordinator_actor.cpp

# src/actor/durable/ — new subdirectory
# passivation_manager.cpp → linked into hpactor_actor

# src/config/parsers/ — follow existing self-registering parser pattern
# reliable_messaging_config_parser.cpp
# durable_config_parser.cpp
```

### 5.7 Header Placement (per CLAUDE.md rules)

| Concern | Directory |
|---------|-----------|
| Reliable messaging (mailbox admission, delivery) | `include/hpactor/mailbox/` |
| Reliable ACK wire format | `include/hpactor/net/` |
| Durable actor state (behaviors, passivation, recovery) | `include/hpactor/actor/durable/` |
| Cluster singleton actor | `include/hpactor/cluster/singleton/` |
| Cluster shard coordinator actor | `include/hpactor/cluster/sharding/` |

---

## 6. PR Structure & Dependency Graph

```
PR #1  MSG-003 Reliable Messaging
         │  (independent — uses existing DeliveryMode, DedupCache, DLQ)
         │
         ▼
PR #2  CLU-003 Singleton Actor Integration
         │  (depends on CLU-001 from Sprint 2, uses MSG-003 ACK for fencing messages)
         │
         ▼
PR #3  DUR-001/002 Durable Actor Runtime
         (depends on CLU-003 for shard-handoff passivation trigger;
          uses DurableStateStore from earlier work)
```

Each PR can ship independently — later PRs build on earlier ones but none are
blocking.

---

## 7. Estimated Scope

| PR | Subsystem | New Headers | New Sources | New Test Files | Est. Tests | Est. LOC |
|----|-----------|-------------|-------------|----------------|------------|----------|
| 1 | MSG-003 | 4 | 4 | 5 | ~38 | ~1800 |
| 2 | CLU-003 | 2 | 2 | 4 | ~27 | ~1200 |
| 3 | DUR-001/002 | 5 | 1 | 5 | ~40 | ~2000 |
| — | Config parsers | — | 2 | — | — | ~200 |
| — | Modified existing | 6 files | 5 files | — | — | ~300 |
| **Total** | | **11** | **9+2** | **14** | **~105** | **~5500** |

---

## 8. Out of Scope (Sprint 4+)

- Distributed pub-sub (requires CLU-003 singleton mediator + Receptionist
  integration)
- Cluster Receptionist (requires CLU-003 + Sprint 1 Receptionist)
- Leader election interface beyond `OldestNodeElection` (Raft, etcd, Consul)
- Per-exception supervision nesting
- Per-actor dispatcher assignment
- Cross-actor schedule
- Admin API (REST/gRPC) — OPS-002
- Authorization + Audit — SEC-002
- Secrets redaction — SEC-003
- Dynamic config reload — OPS-003
- Reactive streams — MSG-004
- Load-aware placement strategy for sharding
- Multi-zone placement metadata (CLU-004)
- External coordinator integration (etcd/Consul)
- `BehaviorTestKit` full `FakeContext` completion
- `TestInbox` helper
- `PipeTo` pattern
- `DeathPact` pattern
- `withStopChildren(false)` supervisor option

---

## 9. Key Design Decisions

1. **ACK after admission, not after handler completion.** Proves message entered
   the runtime without coupling sender to handler latency. Standard distributed
   messaging pattern (Kafka, RabbitMQ, Akka all do this).

2. **No new protobuf types for ACK/NACK.** 14-byte compact binary payload with
   frame flag bits. Avoids protobuf overhead for high-frequency control messages.

3. **`SingletonManagerCore` stays standalone.** The mutex-protected core class is
   tested directly. The `SingletonManagerActor` EventBasedActor wraps it for
   cluster event handling. This keeps the election logic testable without an
   ActorSystem.

4. **`ShardCoordinatorActor` is the first singleton consumer.** Validates the
   entire singleton infrastructure end-to-end with a real use case rather than a
   toy example.

5. **`DurableBehavior<State>` is a template, not a base class.** Avoids virtual
   dispatch on every snapshot/recovery call. User specializes serialization
   methods. No RTTI, no `dynamic_cast`.

6. **PassivationManager is a system actor, not a thread.** Actor lifecycle
   management via message passing fits the actor model. Periodic idle scan uses
   `context->schedule()` for timer-based wakeup.

7. **No consensus for singleton election.** Oldest-node-wins is deterministic and
   sufficient for phase 1. Raft/Paxos/etcd are future pluggable strategies behind
   `ISingletonElection`.

8. **FileDeliveryStore reuses FileStateStore patterns.** Atomic rename, CRC32C
   checksums, per-node subdirectories — same proven approach, different data
   model.
