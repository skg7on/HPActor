# Akka Gap Closure Sprint 2 — Cluster Abstractions Design

## 1. Executive Summary

Sprint 2 of the Akka Typed Actors gap closure (issue #329) focuses on **Cluster
Abstractions (P1)** — the control-plane capabilities needed for safe multi-node
operation. Sprint 1 (#344, merged) closed the developer-ergonomics gaps; Sprint 2
builds the distributed-system foundation that sharding, singletons, and reliable
messaging depend on.

This sprint implements 4 subsystems across 4 sequential PRs, plus produces
design documents for 6 additional items that will be implemented in future
sprints.

### Implementations (this sprint)

| # | Subsystem | Design Doc | Dependencies |
|---|-----------|------------|--------------|
| 1 | **CLU-001** Cluster Failure Model | `docs/architecture/production/cluster-failure-model-design.md` | None (GossipMembership feeds events) |
| 2 | **CLU-002** Cluster Sharding | `docs/architecture/production/cluster-sharding-placement-design.md` | CLU-001 |
| 3 | **CLU-003** Cluster Singleton | Feature gap backlog (CLU-003 card) | CLU-001, CLU-002 |
| 4 | **MSG-003** Reliable Messaging | Feature gap backlog (MSG-003 card) | DeliveryMode, DedupCache (existing) |

### Designs-Only (this sprint)

| # | Item | Implementation | Reason |
|---|------|---------------|--------|
| 5 | Distributed Pub-Sub | Future sprint | Depends on CLU-003 (mediator singleton) |
| 6 | Cluster Receptionist | Future sprint | Depends on CLU-003 + Sprint 1 Receptionist |
| 7 | Leader Election | Future sprint | Pluggable interface; bully algorithm sufficient for singleton failover |
| 8 | Per-Exception Supervision Nesting | Future sprint | Independent; ergonomics improvement |
| 9 | Per-Actor Dispatcher Assignment | Future sprint | Independent; config/spawn change |
| 10 | Cross-Actor Schedule | Future sprint | Independent; scheduler API extension |

## 2. CLU-001: Cluster Failure Model & Fencing

### 2.1 Node State Machine

```cpp
enum class ClusterNodeState : uint8_t {
    Joining,        // New node discovered, not yet trusted for placement
    Alive,          // Node can receive new work
    Suspect,        // Failure detector is probing
    Unreachable,    // Cannot be contacted from this node's perspective
    Quarantined,    // Identity/epoch conflict makes communication unsafe
    Leaving,        // Graceful shutdown has begun
    Down,           // Considered failed for routing and placement
    Removed,        // No longer part of cluster metadata
};
```

State transition diagram:

```
Joining ──handshake complete──► Alive
Alive ──gossip suspect────────► Suspect
Suspect ──probe succeeds──────► Alive
Suspect ──probe timeout───────► Unreachable
Alive ──graceful leave────────► Leaving
Alive ──partition─────────────► Unreachable
Unreachable ──probe succeeds──► Alive
Unreachable ──prolonged────────► Down
Alive ──duplicate identity────► Quarantined
Suspect ──duplicate identity──► Quarantined
Leaving ──drain complete──────► Down
Quarantined ──operator clear──► Joining (re-join required)
Down ──tombstone expiry───────► Removed
```

**Invariants:**
- Only `Alive` accepts new actor placement and user traffic.
- `Quarantined` → only operator action or deterministic tie-breaker resolves. No automatic recovery.
- `Suspect` is time-bounded: probe timeout promotes to `Unreachable`.
- `Down` triggers route invalidation and in-flight failure. Irreversible for that incarnation.
- State transitions are monotonic for a given incarnation.

### 2.2 Node Identity & Fencing

```cpp
struct ClusterNodeIdentity {
    NodeId node_id;              // stable logical name
    uint64_t incarnation;        // monotonic per-start, higher wins
    uint64_t process_start_id;   // boot-id or monotonic counter per process start
    uint64_t membership_epoch;   // cluster membership generation
    ClusterId cluster_id;        // cluster namespace
};
```

**Fencing rules:**

| Condition | Action |
|-----------|--------|
| Same `node_id`, higher `incarnation` | Fence old connections. Old incarnation's pending ops fail with `NodeReplaced`. |
| Same `node_id`, same `incarnation`, different `process_start_id` | Both nodes quarantined. Operator must resolve. |
| Wrong `cluster_id` | Rejected at transport handshake. Never enters membership. |
| Stale `membership_epoch` | Node must re-join before receiving placement or user traffic. |

### 2.3 Partition Policy

```cpp
enum class PartitionPolicy : uint8_t {
    FailOpen,          // Continue best-effort to reachable nodes
    FailClosed,        // Stop remote delivery unless quorum known
    StaticMajority,    // Only configured majority partition owns coordinator roles
};
```

- **Default for user messages:** `FailOpen` — best-effort delivery continues to reachable nodes.
- **Default for singleton/shard ownership:** `FailClosed` — ownership changes require quorum.

### 2.4 Route Invalidation

When a node exits `Alive` → `Down`, `Quarantined`, or `Removed`:

1. Purge `ActorLocationCache` entries pointing to that node.
2. Fail pending `RemoteSpawn` and `RpcChannel` requests with `FailureReason::NodeUnavailable` or `FailureReason::NodeQuarantined`.
3. Mark `ConnectionPool` endpoint unavailable, close connections.
4. Dead-letter in-flight messages that cannot be retried.
5. Emit `DownMsg` to linked/monitored actors with structured `RemoteDownEvent`.

### 2.5 New Files

| File | Purpose |
|------|---------|
| `include/hpactor/cluster/cluster_node_state.hpp` | `ClusterNodeState` enum, transition table, state predicates |
| `include/hpactor/cluster/cluster_node_identity.hpp` | `ClusterNodeIdentity` struct with fencing fields |
| `include/hpactor/cluster/cluster_failure_model.hpp` | `ClusterFailureModel` class — policy engine |
| `include/hpactor/cluster/partition_policy.hpp` | `PartitionPolicy` enum + decision helpers |
| `include/hpactor/cluster/route_invalidation.hpp` | `RouteInvalidation` — cache purge + op failure helper |
| `src/cluster/cluster_failure_model.cpp` | State machine, fencing logic |
| `src/cluster/partition_policy.cpp` | Policy decision logic |
| `src/cluster/route_invalidation.cpp` | Invalidation orchestration |
| `tests/unit/cluster/test_cluster_node_state.cpp` | State transitions, illegal transition rejection |
| `tests/unit/cluster/test_cluster_fencing.cpp` | Incarnation fencing, duplicate identity, cluster-id mismatch |
| `tests/unit/cluster/test_cluster_route_invalidation.cpp` | Cache purge, pending-op failure, connection close |
| `tests/unit/cluster/test_partition_policy.cpp` | Policy decisions per type |

### 2.6 Integration Points

- `ActorSystem` owns `ClusterFailureModel` (created when cluster mode is enabled).
- `GossipMembership` feeds events → `ClusterFailureModel::on_membership_event()`.
- `ActorLocationCache` gains `purge_node(NodeId)` called on invalidation.
- `ConnectionPool` checks `ClusterFailureModel::can_connect(NodeId)` before outbound connects.
- CLI: `/cluster nodes`, `/cluster node <id> show`, `/cluster quarantine`.
- Metrics: `hpactor_cluster_nodes`, `hpactor_cluster_state_transitions_total`, `hpactor_cluster_quarantine_total`.
- `FailureReason` extended with `NodeUnavailable` (new code), `NodeQuarantined` (new code), `NodeReplaced` (new code).

### 2.7 Acceptance Criteria

- All 8 node states are defined with legal transitions documented and tested.
- Duplicate node identity triggers quarantine, not silent conflict.
- Route invalidation is consistent across ActorLocationCache, ActorProxy, RPC, and remote spawn.
- Gossip events feed the failure model but do not directly mutate placement.
- CLI can inspect node state, transition history, and quarantine reason for any node.

---

## 3. CLU-002: Cluster Sharding & Placement

### 3.1 Core Concepts

```
LogicalActorId ("tenant-42/session-9")   ← application-level stable identity
        │
        ▼
  ShardResolver (deterministic hash)     ← maps logical ID → shard ID
        │
        ▼
  ShardTable (shard_id → owner node + epoch)  ← local cached ownership
        │
        ├── owner is local? → LocalActorRegistry
        └── owner is remote? → ActorProxy (remote send via transport)
```

**Shard:** A partition of the logical actor keyspace. Each shard has exactly one active owner at any time.

**ShardCoordinator:** A system actor that owns shard metadata and movement decisions. Runs as a cluster singleton (CLU-003).

### 3.2 ShardResolver

Deterministic mapping from `LogicalActorId` → shard ID:

```cpp
struct LogicalActorId {
    std::string persistence_id;  // e.g., "tenant-42/session-9"
};

class ShardResolver {
public:
    static auto resolve(LogicalActorId id, uint32_t total_shards) -> ShardId;
    // Implementation: std::hash<std::string>{}(id.persistence_id) % total_shards
};
```

### 3.3 ShardTable

Local cache of `(shard_id → owner_node, epoch)`. Invalidated on:
- `ShardMoved` control frame from stale owner.
- Epoch bump from coordinator.
- Node-down event for cached owner (CLU-001).

```cpp
class ShardTable {
public:
    auto lookup(ShardId shard) -> std::optional<ShardEntry>;
    auto update(ShardId shard, NodeId owner, uint64_t epoch) -> void;
    auto invalidate_for_node(NodeId node) -> void;
    auto epoch() const -> uint64_t;

private:
    std::unordered_map<ShardId, ShardEntry> entries_;
    uint64_t current_epoch_ = 0;
};
```

### 3.4 ShardCoordinator

System actor (singleton via CLU-003) responsible for:

- Owning the authoritative shard table.
- Executing placement strategy to assign shards to nodes.
- Orchestrating handoff protocol.
- Publishing epoch updates.

```cpp
class ShardCoordinator : public EventBasedActor {
public:
    void on_register_actor(LogicalActorId id, ActorId actor);
    void on_handoff_complete(ShardId shard, bool success);

private:
    void rebalance();  // Called on node join/leave
    void publish_shard_table();
};
```

### 3.5 Placement Strategy

Interface:

```cpp
class IPlacementStrategy {
public:
    virtual ~IPlacementStrategy() = default;
    // Compute shard→node assignment given current nodes and existing assignments
    virtual auto plan(
        std::span<const ShardId> shards,
        std::span<const NodeId> alive_nodes,
        std::span<const ShardEntry> current_assignments
    ) -> PlacementPlan = 0;
};
```

Phase 1 strategies:

- **`StaticPlacement`** — TOML-defined `[cluster.shards.<id>] owner = "<node>"`. Zero runtime logic.
- **`RendezvousHash`** — Weighted highest-random-weight hash. Deterministic, minimizes movement on node add/remove. Same approach as Akka's consistent-hashing placement.

`LoadAware` deferred to future (requires metrics integration).

### 3.6 Shard Handoff Protocol

States per shard: `Owned` → `Draining` → `Transferring` → `Recovering` → `Active`

```
1. Coordinator selects new owner
2. Old owner → Draining: stops accepting new user messages
3. New messages buffered, rejected, or proxied by policy
4. Old owner snapshots or passivates shard actors (if durable state enabled)
5. New owner → Recovering: rebuilds actor state from snapshots
6. Coordinator publishes new shard table epoch
7. ActorLocationCache entries for moved actors are invalidated
```

### 3.7 ShardMoved Control Frame

When a node receives a message for a shard it no longer owns, it replies with `ShardMoved`:

```protobuf
message PbShardMoved {
    uint32 shard_id = 1;
    string new_owner_node_id = 2;
    uint64 new_epoch = 3;
}
```

Sender updates its local `ShardTable` and re-delivers.

### 3.8 New Files

| File | Purpose |
|------|---------|
| `include/hpactor/cluster/sharding/logical_actor_id.hpp` | `LogicalActorId` type |
| `include/hpactor/cluster/sharding/shard_types.hpp` | `ShardId`, `ShardEntry` types |
| `include/hpactor/cluster/sharding/shard_resolver.hpp` | ID → shard mapping |
| `include/hpactor/cluster/sharding/shard_table.hpp` | Local shard→owner cache with epoch |
| `include/hpactor/cluster/sharding/shard_coordinator.hpp` | Coordinator system actor |
| `include/hpactor/cluster/sharding/placement_strategy.hpp` | `IPlacementStrategy` interface |
| `include/hpactor/cluster/sharding/static_placement.hpp` | Static TOML placement |
| `include/hpactor/cluster/sharding/rendezvous_hash.hpp` | Rendezvous hash placement |
| `include/hpactor/cluster/sharding/shard_handoff.hpp` | Handoff state machine |
| `src/cluster/sharding/shard_resolver.cpp` | Hash implementation |
| `src/cluster/sharding/shard_table.cpp` | Cache implementation |
| `src/cluster/sharding/shard_coordinator.cpp` | Coordinator actor |
| `src/cluster/sharding/static_placement.cpp` | Static placement |
| `src/cluster/sharding/rendezvous_hash.cpp` | Rendezvous hash |
| `src/cluster/sharding/shard_handoff.cpp` | Handoff FSM |
| `tests/unit/cluster/sharding/test_shard_resolver.cpp` | Deterministic mapping tests |
| `tests/unit/cluster/sharding/test_shard_table.cpp` | Cache update + invalidation |
| `tests/unit/cluster/sharding/test_placement_strategy.cpp` | Static + rendezvous hash |
| `tests/unit/cluster/sharding/test_shard_handoff.cpp` | FSM transitions |
| `tests/integration/cluster/sharding/test_shard_coordinator.cpp` | End-to-end coordinator |

### 3.9 Acceptance Criteria

- Logical actor IDs map deterministically to shards.
- Shard ownership changes invalidate stale ActorLocationCache entries.
- Node loss (CLU-001 Down) triggers shard reassignment.
- Handoff drain/recover cycle is observable via metrics and CLI.
- `ShardMoved` control frame causes sender to update cached route.
- CLI: `/cluster shards`, `/cluster shard <id> show`.

---

## 4. CLU-003: Cluster Singleton

### 4.1 Core Concept

Ensures exactly one active owner exists for a given singleton identity across
the cluster. Built on CLU-001 fencing and CLU-002 sharding infrastructure.

### 4.2 Architecture

```
SingletonManager (system actor, per-node)
        │
        ├── owns: map<SingletonIdentity, SingletonProxy>
        │
        ▼
ClusterFailureModel::on_node_state_change()
        │
        ▼
SingletonElection (oldest-node-wins by default)
        │
        ├── I'm owner? → activate singleton actor locally
        └── I'm standby? → monitor owner, wait for failover
```

### 4.3 Key Types

```cpp
struct SingletonIdentity {
    std::string name;            // logical name (e.g., "shard-coordinator")
    uint64_t fencing_token;      // increments on each ownership change
};

enum class SingletonState : uint8_t {
    Standby,      // Not the owner, monitoring owner health
    Activating,   // Becoming owner (transitional)
    Active,       // Current owner
    Draining,     // Graceful handoff in progress
};

class SingletonManager : public EventBasedActor {
public:
    void on_register_singleton(SingletonIdentity id, SpawnConfig config);
    void on_node_state_change(NodeId node, ClusterNodeState old_state,
                              ClusterNodeState new_state);
    void on_fencing_token_rejected(SingletonIdentity id, uint64_t token);

private:
    void run_election(SingletonIdentity id);
    void activate_singleton(SingletonIdentity id);
    void deactivate_singleton(SingletonIdentity id);
};
```

### 4.4 Election Strategy

Phase 1: **oldest-node-wins** (lowest `NodeId` string comparison among `Alive` nodes).
Simple, deterministic, no consensus needed.

Pluggable interface for future strategies:

```cpp
class ISingletonElection {
public:
    virtual ~ISingletonElection() = default;
    virtual auto elect(
        SingletonIdentity id,
        std::span<const NodeId> alive_nodes
    ) -> std::optional<NodeId> = 0;
};
```

Future: external coordinator (etcd/Consul), Raft-based, operator-forced.

### 4.5 Fencing

Every singleton action carries `(singleton_id, fencing_token)`. If a message
arrives with a stale fencing token, it's rejected with `FailureReason::FencingTokenStale`.
This prevents split-brain writes from a deposed owner that hasn't realized it
yet.

### 4.6 New Files

| File | Purpose |
|------|---------|
| `include/hpactor/cluster/singleton/singleton_identity.hpp` | `SingletonIdentity` type |
| `include/hpactor/cluster/singleton/singleton_state.hpp` | `SingletonState` enum |
| `include/hpactor/cluster/singleton/singleton_election.hpp` | `ISingletonElection` interface |
| `include/hpactor/cluster/singleton/oldest_node_election.hpp` | Oldest-node-wins impl |
| `include/hpactor/cluster/singleton/singleton_manager.hpp` | Per-node manager actor |
| `src/cluster/singleton/oldest_node_election.cpp` | Election logic |
| `src/cluster/singleton/singleton_manager.cpp` | Manager actor implementation |
| `tests/unit/cluster/singleton/test_singleton_election.cpp` | Election tests |
| `tests/unit/cluster/singleton/test_singleton_manager.cpp` | Manager behavior tests |
| `tests/integration/cluster/singleton/test_singleton_fencing.cpp` | Fencing token tests |

### 4.7 Integration

- `ShardCoordinator` (CLU-002) is the first consumer — runs as a cluster singleton.
- `SingletonManager` watches `ClusterFailureModel` for node state changes.
- Fencing tokens flow through `FailureEnvelope` on rejected writes.
- `FailureReason` extended with `FencingTokenStale` (new code).

### 4.8 Acceptance Criteria

- At most one active singleton owner exists per singleton identity.
- Ownership changes are audited (log + metrics).
- Conflicting owners (same identity, different nodes) are fenced via token rejection.
- Failover occurs when owner node transitions to Down (via CLU-001).
- CLI: `/cluster singletons`, `/cluster singleton <name> show`.

---

## 5. MSG-003: Reliable Messaging (ACK/NACK)

### 5.1 Core Concept

Opt-in at-least-once delivery for user messages. Built on existing `DeliveryMode`,
`FailureEnvelope`, `DedupCache`, and `MultiLaneQueue`.

### 5.2 Architecture

```
Sender                              Receiver
  │                                    │
  ├── send(msg, mode=AT_LEAST_ONCE)    │
  │                                    │
  ▼                                    │
OutboundTracker                        │
  ├── assigns message_id               │
  ├── stores pending entry             │
  ├── starts retry timer          ────►│ DedupCache.check(message_id)
  │                                    │   ├── duplicate? → ACK + drop
  │      ◄──── ACK ────────────────────│   └── new? → enqueue + ACK
  │                                    │
  │      ◄──── NACK(retry_after) ──────│ (receiver overloaded)
  │                                    │
  ├── retry timer fires                │
  ├── resend (up to max_retries)       │
  │                                    │
  ├── retry exhaustion                 │
  └── → DLQ + FailureEnvelope          │
```

### 5.3 OutboundTracker

```cpp
struct OutboundTrackerEntry {
    MessageId message_id;
    ActorAddr target;
    StreamBuffer payload;
    uint32_t retry_count = 0;
    MonotonicClock::time_point next_retry_at;
    MonotonicClock::time_point deadline;
};

class OutboundTracker {
public:
    // Register a pending reliable send. Returns false if capacity exhausted.
    auto track(MessageId msg_id, ActorAddr target, StreamBuffer payload,
               MonotonicClock::time_point deadline) -> bool;

    // ACK received — remove entry, signal completion.
    auto on_ack(MessageId msg_id) -> void;

    // NACK received — reschedule with retry_after delay.
    auto on_nack(MessageId msg_id, Duration retry_after) -> void;

    // Called periodically. Retries expired entries. DLQs exhausted entries.
    auto tick(MonotonicClock::time_point now) -> void;

    auto pending_count() const -> size_t;

private:
    // Per-destination bounded map of pending entries.
    // Bounded to prevent unbounded memory growth during network partition.
    static constexpr size_t kMaxPendingPerDestination = 1024;
    std::unordered_map<MessageId, OutboundTrackerEntry> entries_;
};
```

### 5.4 Wire Protocol

Two new frame flag bits on the existing Frame header:

- `AckRequested (0x10)` — sender requests ACK/NACK.
- `AckResponse (0x20)` — this frame is an ACK or NACK.

ACK/NACK frames carry `(message_id, status)` in a compact binary payload. No new
protobuf message type needed.

```cpp
enum class AckStatus : uint8_t {
    Accepted = 0,       // Message admitted to receiver mailbox
    Rejected = 1,       // Receiver rejected (full, draining, policy)
    Duplicate = 2,      // Already seen, suppressed
};

struct AckPayload {
    MessageId message_id;
    AckStatus status;
    Duration retry_after;  // 0 if not applicable
};
```

### 5.5 Receiver Integration

`EventBasedActor::receive()` already has `DedupCache` integration from Sprint 1
delivery semantics. Reliable delivery extends receiver behavior:

- **Auto-ACK** on successful dedup check pass (message is new).
- **Auto-ACK** on duplicate detection (avoids unnecessary retransmission).
- **Auto-NACK** if mailbox depth exceeds high watermark, with `retry_after` hint.
- **Auto-NACK** if actor is `Draining` with `DropUserMessages` policy active.
- **No ACK** if `AckRequested` flag is absent (best-effort message).

### 5.6 Retry Policy

```cpp
struct ReliableRetryPolicy {
    uint32_t max_retries = 3;
    Duration initial_backoff = std::chrono::milliseconds(100);
    Duration max_backoff = std::chrono::seconds(10);
    double backoff_multiplier = 2.0;
};
```

Exponential backoff: 100ms → 200ms → 400ms → ... capped at 10s.

On retry exhaustion: message is dead-lettered with `FailureReason::RetryExhausted`,
and a `FailureEnvelope` is emitted with full correlation metadata.

### 5.7 New Files

| File | Purpose |
|------|---------|
| `include/hpactor/mailbox/outbound_tracker.hpp` | `OutboundTracker` class |
| `include/hpactor/mailbox/reliable_retry_policy.hpp` | `ReliableRetryPolicy` struct |
| `include/hpactor/net/reliable_ack.hpp` | `AckStatus`, `AckPayload` types |
| `src/mailbox/outbound_tracker.cpp` | Tracker implementation |
| `src/net/reliable_ack.cpp` | ACK/NACK frame encode/decode |
| `tests/unit/mailbox/test_outbound_tracker.cpp` | Tracker: track, ack, nack, expiry |
| `tests/unit/mailbox/test_reliable_retry_policy.cpp` | Backoff algorithm tests |
| `tests/integration/mailbox/test_reliable_messaging.cpp` | End-to-end ACK/NACK flow |
| `tests/unit/net/test_reliable_ack.cpp` | ACK/NACK frame serialization |

### 5.8 Modified Files

| File | Change |
|------|--------|
| `include/hpactor/actor/event_based_actor.hpp` | Auto-ACK on dedup check in `receive()` |
| `src/actor/event_based_actor.cpp` | ACK/NACK emission logic |
| `include/hpactor/net/frame.hpp` | New frame flags `AckRequested`, `AckResponse` |
| `include/hpactor/msg/type_tag.hpp` | `FailureReason::RetryExhausted` addition |
| `src/config/parsers/` | New `reliable_messaging_config_parser.cpp` for TOML `[system.reliable]` |

### 5.9 TOML Configuration

```toml
[system.reliable]
enabled = true                # default false — opt-in
max_retries = 3
initial_backoff_ms = 100
max_backoff_ms = 10000
backoff_multiplier = 2.0
max_pending_per_destination = 1024
```

### 5.10 Acceptance Criteria

- AT_LEAST_ONCE delivery mode triggers OutboundTracker registration.
- ACK removes pending entry. NACK reschedules.
- Retry with exponential backoff stops at max_retries.
- Retry exhaustion creates DLQ record with `FailureReason::RetryExhausted`.
- Duplicate message_id is suppressed at receiver, ACK sent anyway.
- Best-effort messages (no AckRequested flag) are unaffected.
- Bounded capacity: `max_pending_per_destination` enforced.

---

## 6. Design-Only Items

These items receive architecture designs in this sprint but implementation is
deferred to future sprints. Each design is summarized below; full design docs
will be written as standalone files in `docs/architecture/production/`.

### 6.1 Distributed Pub-Sub

Topic-based publish/subscribe across cluster nodes.

**Architecture:**
- `Topic<T>` — typed topic identifier (protobuf `TypeTag`-aware).
- `PubSubMediator` — cluster singleton (uses CLU-003). Maintains `topic → set<ActorId>` registry.
- Per-node `PubSubProxy` — local actor that forwards publish/subscribe to the mediator.
- `context->publish(topic, msg)` — fire-and-forget publish.
- `context->subscribe(topic)` / `context->unsubscribe(topic)` — register interest.
- Node-down events (CLU-001) auto-unsubscribe dead actors.

**Design doc target:** `docs/architecture/production/distributed-pub-sub-design.md`

### 6.2 Cluster Receptionist

Extends Sprint 1 `Receptionist` (local `ServiceKey`-based actor discovery) across
the cluster.

**Architecture:**
- `ClusterReceptionist` system actor — singleton via CLU-003.
- Subscribes to local Receptionist changes.
- Gossips `(ServiceKey, ActorId, NodeId)` tuples to remote peers.
- Remote subscribers receive `Listing` updates when cross-node registrations change.
- Uses CLU-001 route invalidation to remove dead actors from listings.

**Design doc target:** `docs/architecture/production/cluster-receptionist-design.md`

### 6.3 Leader Election

Pluggable leader election for coordinator roles (singleton failover, shard
coordinator).

**Architecture:**
```cpp
class ILeaderElection {
public:
    virtual ~ILeaderElection() = default;
    virtual auto elect(ContenderId me, std::vector<ContenderId> peers)
        -> ElectionResult = 0;
    virtual auto on_peer_down(ContenderId peer) -> void = 0;
};
```
- Phase 1: **bully algorithm** — highest node-id wins among Alive nodes. Deterministic, no consensus.
- Future: Raft-based, external coordinator (etcd/Consul), operator-forced.

**Design doc target:** `docs/architecture/production/leader-election-design.md`

### 6.4 Per-Exception Supervision Nesting

Allow supervisors to map different failure causes to different strategies.

**Architecture:**
```cpp
behavior
  .on_failure<TimeoutException>(SupervisionStrategy::restart())
  .on_failure<OOMException>(SupervisionStrategy::stop())
  .on_failure<ValidationError>(SupervisionStrategy::resume());
```
- Extends `SelfSupervisingActor` with `std::vector<FailureMapping>` — `(FailureReason predicate, SupervisionAction action)` pairs.
- On child failure, walk the list; first match wins. Default: existing strategy.
- No runtime type dispatch — uses `FailureReason` enum values, not `typeid`/RTTI.

**Design doc target:** `docs/architecture/production/per-exception-supervision-design.md`

### 6.5 Per-Actor Dispatcher Assignment

Allow individual actors to specify which dispatcher/thread pool they execute on.

**Architecture:**
```toml
[[actors]]
name = "compute-heavy"
behavior = "DenseCompute"
dispatcher = "dedicated-cpu-pool"
```
- `DispatcherConfig` already exists in `TopologyModel`.
- Extend `ActorSystem::spawn_configured()` to route to named dispatcher.
- Existing `DenseComputingActor` / `DaemonActor` patterns validate concept.
- Default: `HybridScheduler` (no behavior change for existing configs).

**Design doc target:** `docs/architecture/production/per-actor-dispatcher-design.md`

### 6.6 Cross-Actor Schedule

Extend `context->schedule(delay, msg)` to support target actors other than self.

**Architecture:**
```cpp
auto handle = context->schedule(target_actor, delay, msg);
```
- `ActorContext::schedule()` overload with `ActorAddr target` parameter.
- `TimingWheel` fires `ScheduledDelivery` system message.
- If target != self, scheduler enqueues to target's mailbox.
- `cancel_schedule(handle)` works unchanged — `AlarmHandle` already encodes timer identity.
- Permission check: cross-actor scheduling allowed only within same supervision tree (default) or via explicit config.

**Design doc target:** `docs/architecture/production/cross-actor-schedule-design.md`

### 6.7 Acceptance Criteria (Design-Only)

- Each item has a standalone design doc in `docs/architecture/production/`.
- Existing architecture docs (`production-reliability-plane.md`, `feature-gap-refined-requirement-backlog.md`) are updated with references.
- No implementation code written — designs are complete and reviewable.

---

## 7. Cross-Cutting Concerns

### 7.1 New Public Header Directory

All cluster subsystem headers live under `include/hpactor/cluster/`:

```
include/hpactor/cluster/
├── cluster_node_state.hpp
├── cluster_node_identity.hpp
├── cluster_failure_model.hpp
├── partition_policy.hpp
├── route_invalidation.hpp
├── sharding/
│   ├── logical_actor_id.hpp
│   ├── shard_types.hpp
│   ├── shard_resolver.hpp
│   ├── shard_table.hpp
│   ├── shard_coordinator.hpp
│   ├── placement_strategy.hpp
│   ├── static_placement.hpp
│   ├── rendezvous_hash.hpp
│   └── shard_handoff.hpp
└── singleton/
    ├── singleton_identity.hpp
    ├── singleton_state.hpp
    ├── singleton_election.hpp
    ├── oldest_node_election.hpp
    └── singleton_manager.hpp
```

### 7.2 Source Directory

```
src/cluster/
├── cluster_failure_model.cpp
├── partition_policy.cpp
├── route_invalidation.cpp
├── sharding/
│   ├── shard_resolver.cpp
│   ├── shard_table.cpp
│   ├── shard_coordinator.cpp
│   ├── static_placement.cpp
│   ├── rendezvous_hash.cpp
│   └── shard_handoff.cpp
└── singleton/
    ├── oldest_node_election.cpp
    └── singleton_manager.cpp
```

### 7.3 Test Directory

```
tests/unit/cluster/
├── test_cluster_node_state.cpp
├── test_cluster_fencing.cpp
├── test_cluster_route_invalidation.cpp
├── test_partition_policy.cpp
├── sharding/
│   ├── test_shard_resolver.cpp
│   ├── test_shard_table.cpp
│   ├── test_placement_strategy.cpp
│   └── test_shard_handoff.cpp
└── singleton/
    ├── test_singleton_election.cpp
    └── test_singleton_manager.cpp
```

### 7.4 Existing Code Changes

| File | Change | Reason |
|------|--------|--------|
| `include/hpactor/core/actor_system.hpp` | Own `ClusterFailureModel` member | CLU-001 integration |
| `src/actor/actor_system.cpp` | Create `ClusterFailureModel` on cluster init | CLU-001 integration |
| `include/hpactor/net/actor_location_cache.hpp` | Add `purge_node(NodeId)` method | CLU-001 route invalidation |
| `src/net/actor_location_cache.cpp` | Purge implementation | CLU-001 route invalidation |
| `include/hpactor/net/connection_pool.hpp` | Check `can_connect()` before outbound | CLU-001 fencing |
| `include/hpactor/actor/event_based_actor.hpp` | Auto-ACK on dedup in `receive()` | MSG-003 |
| `src/actor/event_based_actor.cpp` | ACK/NACK emission logic | MSG-003 |
| `include/hpactor/net/frame.hpp` | Add `AckRequested`, `AckResponse` flags | MSG-003 |
| `include/hpactor/msg/type_tag.hpp` | New `FailureReason` codes | CLU-001, CLU-003, MSG-003 |
| `tests/support/system_test_fixture.hpp` | Cluster mode helpers | All |

### 7.5 FailureReason Additions

| Code | Name | Subsystem |
|------|------|-----------|
| TBD | `NodeUnavailable` | CLU-001 — node is Down or Removed |
| TBD | `NodeQuarantined` | CLU-001 — node identity conflict |
| TBD | `NodeReplaced` | CLU-001 — fenced by higher incarnation |
| TBD | `FencingTokenStale` | CLU-003 — singleton ownership changed |
| TBD | `RetryExhausted` | MSG-003 — reliable message retry limit reached |
| TBD | `ShardNotOwned` | CLU-002 — shard moved, message redirected |

### 7.6 Deterministic Testing Contract

All tests follow `.claude/rules` testing constraints:

- **No real threads** in unit tests. Use `SchedulerTestDriver` with worker count = 0 where scheduler-dependent behavior needs observation.
- **No sleep/wall-clock timing assumptions.** Use condition-based polling with generous timeouts (5s+) only in integration tests.
- **State machine tests** verify transitions programmatically — no reliance on thread interleaving.
- **Fencing tests** inject identity conflicts via direct `ClusterFailureModel` method calls, not by spawning real duplicate nodes.
- **Shard tests** use `mailbox->inject_for_test()` for deterministic message observation.
- **Reliable messaging tests** use a controlled `tick()` loop, not real timers.

### 7.7 Build & CMake

New `src/cluster/CMakeLists.txt` with targets for `cluster_failure_model`,
`sharding`, and `singleton` libraries, linked into `hpactor_lib`.

```cmake
add_library(hpactor_cluster STATIC
    cluster_failure_model.cpp
    partition_policy.cpp
    route_invalidation.cpp
    sharding/shard_resolver.cpp
    sharding/shard_table.cpp
    sharding/shard_coordinator.cpp
    sharding/static_placement.cpp
    sharding/rendezvous_hash.cpp
    sharding/shard_handoff.cpp
    singleton/oldest_node_election.cpp
    singleton/singleton_manager.cpp
)
target_link_libraries(hpactor_cluster PUBLIC hpactor_core)
```

---

## 8. Observability Summary

### Metrics

| Metric | Subsystem |
|--------|-----------|
| `hpactor_cluster_nodes{state}` | CLU-001 |
| `hpactor_cluster_state_transitions_total` | CLU-001 |
| `hpactor_cluster_quarantine_total` | CLU-001 |
| `hpactor_cluster_route_invalidations_total` | CLU-001 |
| `hpactor_shards_total` | CLU-002 |
| `hpactor_shard_moves_total` | CLU-002 |
| `hpactor_shard_handoff_duration_seconds` | CLU-002 |
| `hpactor_shard_route_misses_total` | CLU-002 |
| `hpactor_cluster_singleton_owner` | CLU-003 |
| `hpactor_cluster_singleton_failover_total` | CLU-003 |
| `hpactor_reliable_outbox_pending` | MSG-003 |
| `hpactor_reliable_acks_total` | MSG-003 |
| `hpactor_reliable_retries_total` | MSG-003 |
| `hpactor_reliable_duplicates_total` | MSG-003 |

### CLI Commands

| Command | Subsystem |
|---------|-----------|
| `/cluster nodes` | CLU-001 |
| `/cluster node <id> show` | CLU-001 |
| `/cluster quarantine` | CLU-001 |
| `/cluster shards` | CLU-002 |
| `/cluster shard <id> show` | CLU-002 |
| `/cluster singletons` | CLU-003 |
| `/cluster singleton <name> show` | CLU-003 |
| `/reliable pending` | MSG-003 |
| `/reliable stats` | MSG-003 |

---

## 9. Dependency Graph

```
CLU-001 (Cluster Failure Model)
    │
    ├──► CLU-002 (Sharding)
    │       │
    │       └──► ShardCoordinator runs on CLU-003 singleton
    │
    ├──► CLU-003 (Singleton)
    │       │
    │       ├──► CLU-002 ShardCoordinator (first consumer)
    │       ├──► PubSubMediator (future)
    │       └──► ClusterReceptionist (future)
    │
    └──► MSG-003 (Reliable Messaging) — independent, uses CLU-001 for node-down ACK decisions
```

---

## 10. PR Sequence & Estimated Scope

| PR | Subsystem | New Headers | New Sources | New Test Files | Est. LOC |
|----|-----------|-------------|-------------|----------------|----------|
| 1 | CLU-001 | 5 | 3 | 4 | ~1200 |
| 2 | CLU-002 | 9 | 6 | 5 | ~2000 |
| 3 | CLU-003 | 5 | 2 | 3 | ~1000 |
| 4 | MSG-003 | 3 | 2 | 4 | ~1000 |
| — | Design docs (6) | — | — | — | ~1500 (docs) |

---

## 11. Out of Scope (Sprint 3+)

- Cluster-wide consensus (Raft/Paxos). CLU-001 uses deterministic rules; CLU-003 uses oldest-node-wins.
- Load-aware placement (CLU-002). Static + rendezvous hash only.
- External coordinator integration (etcd/Consul). Pure HPActor-native for Sprint 2.
- Durable outbox/inbox for reliable messaging (DUR-001/DUR-002).
- Multi-zone placement metadata (CLU-004).
- Protocol negotiation (NET-001) — ACK/NACK flags on existing frame format.
- mTLS identity binding (SEC-001) — fencing uses `ClusterNodeIdentity` fields without cert fingerprint.
