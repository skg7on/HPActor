# Cluster Subsystem — High-Level Architecture & Key Concepts

## 1. Executive Summary

The HPActor Cluster Subsystem provides the distributed control-plane capabilities needed
for safe multi-node actor operation. It builds on top of the existing `GossipMembership`
(SWIM-based node discovery) and `ActorSystem` runtime, adding:

- **Failure detection and fencing** — a node state machine that separates membership
  detection from control-plane action, with incarnation-based fencing to prevent
  split-brain conflicts.
- **Sharding and placement** — deterministic partitioning of the logical actor keyspace
  into shards, with pluggable placement strategies and a handoff protocol for safe
  ownership transfer.
- **Cluster singletons** — exactly-one-owner semantics for coordinator roles (shard
  coordinator, pub-sub mediator, receptionist), backed by fencing tokens to reject
  stale-owner writes.
- **Route invalidation** — coordinated cleanup of actor location caches, connection
  pools, pending RPC requests, and in-flight messages when a node fails.

The cluster subsystem is the **control plane** of the production reliability
architecture, sitting between the data plane (message delivery, mailboxes, DLQ) and
the operations plane (health, admin API, incident diagnostics).

### Relationship to Existing Systems

| Existing Capability | Cluster Subsystem Role |
|---------------------|----------------------|
| `GossipMembership` (SWIM) | Feeds membership events into `ClusterFailureModel` for policy-gated state transitions |
| `ActorLocationCache` | Purged on node-down via `RouteInvalidation` callbacks |
| `ConnectionPool` | Checks `ClusterFailureModel::can_connect()` before outbound connects |
| `ActorSystem::spawn()` | Routed through `ShardResolver` → `ShardTable` → local spawn or `ActorProxy` |
| `SingletonManagerActor` | Spawned automatically by `ActorSystem::enable_cluster()`; runs `ShardCoordinatorActor` as its first singleton |

### Position in the HPActor Architecture

```
ActorSystem
├── Data Plane                         Control Plane (Cluster Subsystem)
│   ├── Mailbox / MultiLaneQueue           ├── ClusterFailureModel ◄── GossipMembership
│   ├── Delivery Pipeline / DLQ            ├── PartitionPolicy
│   ├── EventBasedActor receive()          ├── RouteInvalidation
│   └── OutboundTracker (reliable msg)     ├── ClusterNodeIdentity / fencing
│                                          │
│                                          ├── Sharding
│                                          │   ├── ShardResolver (id → shard)
│                                          │   ├── ShardTable (local cache)
│                                          │   ├── ShardCoordinatorCore
│                                          │   │   └── ShardCoordinatorActor (singleton)
│                                          │   ├── IPlacementStrategy
│                                          │   │   ├── StaticPlacement
│                                          │   │   └── RendezvousHash
│                                          │   └── ShardHandoff (FSM)
│                                          │
│                                          └── Singleton
│                                              ├── SingletonIdentity / fencing token
│                                              ├── ISingletonElection
│                                              │   └── OldestNodeElection
│                                              ├── SingletonManagerCore
│                                              └── SingletonManagerActor (system actor)
└── Operations Plane
    ├── Metrics / CLI / Health
    └── Fault Injection / Chaos
```

---

## 2. Design Goals

### G1: Separate Detection from Decision

Gossip membership detects that a node *may* be failing. The cluster failure model
decides what to *do* about it. This separation prevents transient network glitches
from cascading into shard reassignment or singleton failover, and allows different
partition policies per workload type.

### G2: Prevent Split-Brain via Fencing

Every cluster action that affects ownership (singleton activation, shard assignment)
carries an incarnation counter and a fencing token. Stale-token messages are rejected
with explicit failure reasons. Two processes claiming the same node identity are
quarantined, not silently allowed to conflict.

### G3: Deterministic Placement with Minimal Movement

Shard-to-node assignment must be reproducible from the same inputs (shard IDs, alive
nodes, current assignments). The `RendezvousHash` strategy ensures that adding or
removing a node only moves the shards that hash to that node — not the entire keyspace.

### G4: Composability — Core + Actor Wrapper

Thread-safe core classes (`ClusterFailureModel`, `ShardCoordinatorCore`,
`SingletonManagerCore`) are tested directly without an `ActorSystem`. Actor wrappers
(`ShardCoordinatorActor`, `SingletonManagerActor`) add cluster event handling,
lifecycle, and message-driven integration. This pattern keeps business logic testable
with `BehaviorTestKit` while enabling full `ActorSystem` integration.

### G5: Opt-In Cluster Mode

Existing single-node applications are unaffected. Cluster mode is enabled via
`ActorSystem::enable_cluster(node_id)`, which creates the failure model, singleton
manager, and observer wiring. Best-effort local actor spawning works identically
regardless of cluster mode.

### G6: Deterministic Testing

All cluster state machines, election algorithms, fencing logic, and handoff FSMs are
tested without real threads, wall-clock timers, or network I/O. Tests use
`SchedulerTestDriver`, `BehaviorTestKit`, and direct method calls on the core classes.

---

## 3. Key Concepts

### 3.1 Node State Machine

Every node in the cluster has a lifecycle state. The state machine has eight states
with constexpr-verified legal transitions:

```
Joining ──► Alive ◄──► Suspect ◄──► Unreachable ──► Down ──► Removed
               │                         ▲
               ▼                         │
            Leaving ─────────────────────┘
               │
               ▼
          Quarantined ──► Joining (operator clear only)
```

| State | Meaning | Placement | User Traffic |
|-------|---------|-----------|-------------|
| `Joining` | Discovered, not yet trusted | No | No |
| `Alive` | Healthy, fully participating | Yes | Yes |
| `Suspect` | Failure detector probing | Yes (degraded) | Yes |
| `Unreachable` | Cannot be contacted | No | No |
| `Quarantined` | Identity/epoch conflict | No | No |
| `Leaving` | Graceful shutdown in progress | Draining only | Draining only |
| `Down` | Confirmed failed | No | No |
| `Removed` | Tombstone expired | No | No |

**Key invariant:** Only `Alive` nodes accept new actor placement. `Suspect` nodes
continue serving existing traffic while being probed. `Quarantined` requires explicit
operator intervention — no automatic recovery from identity conflicts.

### 3.2 Node Identity & Fencing

Every transport connection and membership advertisement carries a `ClusterNodeIdentity`:

```cpp
struct ClusterNodeIdentity {
    std::string node_id;          // Stable logical name
    uint64_t incarnation;         // Monotonic per-start counter; higher wins
    uint64_t process_start_id;    // Boot-id or process-start counter
    uint64_t membership_epoch;    // Cluster membership generation
    ClusterId cluster_id;         // Cluster namespace
};
```

**Fencing rules:**

| Condition | Action |
|-----------|--------|
| Same `node_id`, higher `incarnation` | Fence old connections. Old incarnation's pending ops fail with `NodeReplaced`. |
| Same `node_id`, same `incarnation`, different `process_start_id` | Both nodes quarantined. Identity conflict — operator must resolve. |
| Wrong `cluster_id` | Rejected at transport handshake. Never enters membership. |
| Stale `membership_epoch` | Node must re-join before receiving placement or user traffic. |

Fencing ensures that a deposed singleton owner cannot perform split-brain writes:
every singleton action message carries `(singleton_name, fencing_token)`, and stale
tokens are rejected with `FailureReason::FencingTokenStale`.

### 3.3 Partition Policy

The cluster's behavior during network partitions is configurable per workload type:

| Policy | User Messages | Ownership Changes |
|--------|--------------|-------------------|
| `FailOpen` (default for user traffic) | Continue best-effort to reachable nodes | Require majority |
| `FailClosed` (default for ownership) | Stop unless quorum known | Require majority |
| `StaticMajority` | Stop unless quorum known | Only configured majority partition |

This allows user-facing best-effort messaging to continue during minor partitions
while preventing singleton/shard ownership from flip-flopping.

### 3.4 Sharding

**Shard:** A partition of the logical actor keyspace. Each shard has exactly one
active owner node at any time. Shards are the unit of placement and movement.

**LogicalActorId:** A stable application-level identity (e.g., `"tenant-42/session-9"`)
that maps deterministically to a `ShardId` via `ShardResolver::resolve()`.

```
LogicalActorId ("tenant-42/session-9")
        │
        ▼
  ShardResolver::resolve()       ← std::hash(id) % total_shards
        │
        ▼
  ShardTable::lookup(shard_id)   ← local cached ownership
        │
        ├── owner is local? → LocalActorRegistry::spawn()
        └── owner is remote? → ActorProxy::send() to owner node
```

### 3.5 Shard Handoff Protocol

When a shard moves from one node to another (rebalance or node failure), a five-state
FSM governs the transition:

```
Owned ──► Draining ──► Transferring ──► Recovering ──► Active
  ▲                        │
  └──── Abort ─────────────┘
```

1. **Draining:** Old owner stops accepting new user messages for the shard. In-flight
   messages complete normally.
2. **Transferring:** Old owner snapshots/passivates durable actors. Ownership metadata
   transfers to new owner.
3. **Recovering:** New owner loads shard state (snapshots + event replay for durable
   actors). Not yet accepting user traffic.
4. **Active:** New owner fully operational. Coordinator publishes new epoch.
   `ActorLocationCache` entries for moved actors are invalidated.

### 3.6 Cluster Singleton

Ensures exactly one active owner exists for a given singleton identity across the
cluster. Used for coordinator roles — `ShardCoordinatorActor` is the first consumer.
Production ownership uses a distributed leadership backend, not only local
alive-node selection. The production target is an external coordinator
(etcd first, Consul-compatible) with an internal Raft backend as a future
implementation. Local strategies such as `OldestNodeElection` remain useful for
tests, single-node development, and explicitly non-production modes.

```
SingletonManagerActor (per-node system actor)
        │
        ├── ClusterFailureModel observer callback
        │   └── on_node_state_change(alive_nodes)
        │
        ├── ClusterLeadershipManagerActor
        │   └── ILeadershipBackend::try_acquire()/renew()
        │
        ├── Lease owner is local? → SingletonState::Activating → Active
        │   └── spawn singleton actor locally, use backend fencing token
        │
        └── I'm standby? → SingletonState::Standby
            └── Watch backend owner; if owner → Down/lost, acquire lease
```

**Fencing token lifecycle:**
- Token starts unset when singleton is registered.
- Production tokens are issued by the leadership backend: etcd revision,
  Consul KV/session generation, or future Raft `(term, log_index)`.
- Every singleton action message carries `(singleton_name, fencing_token)`.
- Messages with stale tokens are rejected with `FailureReason::FencingTokenStale`.

### 3.7 Route Invalidation

When a node exits `Alive` → `Down`, `Quarantined`, or `Removed`, a coordinated
cleanup executes across multiple subsystems via registered callbacks:

1. **ActorLocationCache** — purge all entries pointing to the dead node.
2. **ActorProxy** — fail pending `RemoteSpawn` and `RpcChannel` requests with
   `FailureReason::NodeUnavailable` or `FailureReason::NodeQuarantined`.
3. **ConnectionPool** — mark endpoint unavailable, close connections.
4. **DLQ / OutboundTracker** — dead-letter in-flight reliable messages that cannot
   be retried. `OutboundTracker::fail_pending_for_node()` DLQs all pending sends.
5. **Linked/Monitored Actors** — emit `DownMsg` with structured `RemoteDownEvent`
   so supervision trees can react.

---

## 4. Subsystem Decomposition

### 4.1 Cluster Failure Model (`CLU-001`)

**Purpose:** Node lifecycle state machine, identity fencing, partition policy,
and route invalidation coordination.

| Component | Type | Thread Safety | Description |
|-----------|------|---------------|-------------|
| `ClusterNodeState` | enum | constexpr | 8-state model with `can_transition()` validation |
| `ClusterNodeIdentity` | struct | constexpr helpers | Five-field identity with `fences()`, `is_identity_conflict()`, `same_cluster()`, `has_stale_epoch()` |
| `ClusterFailureModel` | class | Mutex-guarded | Owns the node state map. `register_node()`, `transition()`, `get_state()`, `quorum_present()`, `alive_nodes()`, observer callbacks |
| `PartitionPolicy` | enum | constexpr helpers | `FailOpen`, `FailClosed`, `StaticMajority` with `allow_user_delivery()` and `allow_ownership_change()` decision helpers |
| `RouteInvalidation` | class | Callback-based | `process(node_ids)` iterates registered callbacks for cache purge, op failure, connection close |

**Integration:** `ActorSystem::enable_cluster()` creates a `ClusterFailureModel`
instance. `GossipMembership` feeds membership events. Observer callbacks notify
`SingletonManagerActor` of alive-node changes.

### 4.2 Cluster Sharding (`CLU-002`)

**Purpose:** Deterministic partitioning of the logical actor keyspace, ownership
tracking, placement, and handoff.

| Component | Type | Thread Safety | Description |
|-----------|------|---------------|-------------|
| `LogicalActorId` / `ShardId` / `ShardEntry` | types | — | Core sharding types |
| `ShardResolver` | static class | Stateless | `resolve(LogicalActorId, total_shards) → ShardId` via `std::hash` |
| `ShardTable` | class | Mutex-guarded | Local `shard_id → (owner_node, epoch)` cache. `lookup()`, `update()` (ignores stale epochs), `invalidate_for_node()` |
| `ShardCoordinatorCore` | class | Mutex-guarded | Authoritative shard table + placement strategy. `register_actor()`, `unregister_actor()`, `rebalance(alive_nodes)`, `get_shard_owner()` |
| `ShardCoordinatorActor` | EventBasedActor wrapper | Actor-guaranteed | Wraps `ShardCoordinatorCore`. Handles `RegisterShardActor`, `RebalanceRequest`, `GetShardOwner` messages. Runs as cluster singleton. |
| `IPlacementStrategy` | interface | — | `plan(shards, alive_nodes, current_assignments) → PlacementPlan` |
| `StaticPlacement` | impl | Stateless | TOML-defined static mapping |
| `RendezvousHash` | impl | Stateless | Highest-random-weight hash; minimizes movement |
| `ShardHandoff` | class | Single-owner | 5-state FSM: `begin_drain()`, `complete_drain()`, `begin_recovery()`, `activate()`, `abort()` |

### 4.3 Cluster Singleton (`CLU-003`)

**Purpose:** Exactly-one-owner semantics for coordinator roles across the cluster.

| Component | Type | Thread Safety | Description |
|-----------|------|---------------|-------------|
| `SingletonIdentity` / `SingletonState` | types | — | `{name, fencing_token}` and `{Standby, Activating, Active, Draining}` |
| `ILeadershipBackend` | interface | backend-owned | `try_acquire()`, `renew()`, `release()`, `current_owner()`, `watch()` for production leadership leases |
| `ExternalCoordinatorBackend` | impl | backend-owned | etcd-first production backend; Consul-compatible through the same lease contract |
| `ISingletonElection` | interface | — | Local/non-production strategy seam: `elect(id, alive_nodes) → optional<NodeId>` + `on_peer_down(node_id)` |
| `OldestNodeElection` | impl | Stateless | Lowest `node_id` among `Alive` nodes wins. Deterministic, no consensus; not production ownership. |
| `SingletonManagerCore` | class | Mutex-guarded | Per-node singleton registry. `register_singleton()`, `on_node_state_change(alive_nodes)`, `begin_drain()`, `complete_drain()`, `get_fencing_token()` |
| `SingletonManagerActor` | EventBasedActor wrapper | Actor-guaranteed | Spawned automatically by `enable_cluster()`. Receives `RegisterSingleton`, `NodeStateChange`, `BeginDrain`, `CompleteDrain` messages. Manages singleton lifecycle and fencing token propagation. |

**Production leadership flow:**
1. `ClusterFailureModel` observer callback fires with new `alive_nodes` list.
2. Callback sends membership eligibility to `ClusterLeadershipManagerActor`.
3. Leadership manager acquires or renews a backend `LeadershipLease`.
4. If the committed lease owner is local, `SingletonManagerActor` transitions
   `Standby → Activating → Active` and exposes the backend fencing token.
5. If the lease is lost, expired, or superseded, the active singleton transitions
   `Active → Draining → Standby`.
6. Mutating singleton and shard-coordinator commands reject stale or missing
   tokens.

Detailed design: [Production Distributed Leadership Election Design](../production/distributed-leadership-election-design.md).

### 4.4 Reliable Messaging (`MSG-003`)

**Purpose:** Opt-in at-least-once delivery with ACK/NACK, exponential backoff retry,
and durable outbox for restart recovery.

*Note: While `MSG-003` is primarily a data-plane concern, it integrates with the
cluster subsystem at key touchpoints: node-down triggers `fail_pending_for_node()`,
and durable stores enable restart recovery for pending reliable sends.*

| Component | Type | Description |
|-----------|------|-------------|
| `OutboundTracker` | class | Bounded per-destination pending map. `track()`, `on_ack()`, `on_nack()`, `tick()`, `fail_pending_for_node()` |
| `ReliableRetryPolicy` | struct | Exponential backoff: 100ms → 200ms → 400ms → ... capped at 10s. `max_retries = 3`. |
| `AckStatus` / `AckPayload` | types | Compact 14-byte binary wire format (no protobuf). `Accepted`, `Rejected`, `Duplicate` status. |
| `InMemoryDeliveryStore` | adapter | For tests and non-durable mode. |
| `FileDeliveryStore` | adapter | Atomic rename + CRC32C. Survives process restart. |

---

## 5. Integration: Enabling Cluster Mode

Cluster mode is opt-in. A single-node application that never calls `enable_cluster()`
behaves identically to previous versions. When cluster mode is enabled:

```cpp
// In ActorSystem initialization:
if (cluster_enabled_) {
    // 1. Create ClusterFailureModel — the node state machine
    // 2. Create SingletonManagerActor with election strategy
    // 3. Register "shard-coordinator" as the first managed singleton
    // 4. Wire observer: failure model → singleton manager
    //    (node state changes trigger singleton election re-runs)
}
```

The initialization sequence in `cluster_system_bridge.cpp`:

```
enable_cluster(node_id)
  ├── new ClusterFailureModel()
  ├── new ClusterLeadershipManagerActor(ILeadershipBackend)
  ├── new SingletonManagerActor(node_id, leadership_manager)
  ├── singleton_mgr.register_singleton({"shard-coordinator", fencing_token=unset})
  └── failure_model.register_observer(alive_nodes → leadership_manager.on_membership_change(alive_nodes))
```

**What happens on node state changes:**
1. `GossipMembership` detects membership change.
2. `ClusterFailureModel::transition(node_id, new_state, reason)` is called.
3. If transition is legal and successful, `invalidation_queue_` is populated (for Down/Quarantined/Removed).
4. Observer callbacks fire with updated `alive_nodes()` list.
5. `ClusterLeadershipManagerActor` receives the membership update and acquires,
   renews, or releases backend leadership leases.
6. `SingletonManagerActor` activates or drains local singletons from committed
   lease changes.
7. `RouteInvalidation::process()` drains the invalidation queue.

---

## 6. Design Principles

### P1: Core/Actor Separation

Every cluster subsystem follows a two-layer pattern:

| Layer | Characteristics | Test Strategy |
|-------|----------------|---------------|
| **Core class** (`*Core`, `*Model`) | Thread-safe via mutex. No actor dependency. No message passing. Direct API. | Unit tests with direct method calls. No `ActorSystem` required. |
| **Actor wrapper** (`*Actor`) | EventBasedActor subclass. Handles TypeTag-dispatch messages. Integrates with spawn/stop/lifecycle. | `BehaviorTestKit` for synchronous behavior testing. Integration tests with full `ActorSystem`. |

This enables:
- Testing business logic without spawning an `ActorSystem`.
- The core class can be used in non-actor contexts (e.g., tools, CLI processors).
- Actor integration is thin — mostly message dispatch to core methods.

### P2: No Consensus for Phase 1

Singleton election uses `OldestNodeElection` (deterministic, lowest-node-id-wins).
Shard coordinator is a single singleton. There is no Raft, Paxos, or external
coordinator dependency. The `ISingletonElection` interface is designed for future
pluggability (Raft, etcd, Consul, operator-forced).

This trades perfect availability during partitions for implementation simplicity.
`FailClosed` partition policy on ownership ensures safety: if a node cannot confirm
it's part of the majority, it will not claim singleton or shard ownership.

### P3: Deterministic Testing

All cluster tests follow the `.claude/rules` testing constraints:

- **No real threads** in unit tests — `SchedulerTestDriver` with worker count = 0,
  or `BehaviorTestKit` for synchronous behavior testing.
- **No wall-clock timing assumptions** — controlled `tick()` loops for retry/backoff,
  condition-based polling only in integration tests.
- **State machine tests** verify transitions programmatically — no reliance on
  thread interleaving.
- **Fencing tests** inject identity conflicts via direct `ClusterFailureModel` method
  calls, not by spawning real duplicate nodes.

### P4: Explicit Failure, Not Silent Degradation

Every cluster operation that can fail produces a `FailureReason` enum value, a
`FailureEnvelope` with correlation metadata, and (where applicable) a `DeadLetterRecord`.
Key failure reasons added for cluster:

| FailureReason | Meaning |
|---------------|---------|
| `NodeUnavailable` | Node is Down or Removed |
| `NodeQuarantined` | Node identity conflict detected |
| `NodeReplaced` | Fenced by higher incarnation |
| `FencingTokenStale` | Singleton ownership changed; retry with new owner |
| `ShardNotOwned` | Shard moved; sender should update ShardTable and redeliver |

### P5: Bounded Everything

All cluster data structures are bounded:
- `OutboundTracker`: `kMaxPendingPerDestination = 1024`
- `ShardTable`: bounded by `total_shards` (configurable)
- `ClusterFailureModel::nodes_`: bounded by actual cluster size
- `SingletonManagerCore::singletons_`: bounded by number of registered singletons

No unbounded growth during partitions, flapping nodes, or message storms.

### P6: Epoch-Based Cache Coherence

Shard ownership uses epochs for cache invalidation:
- Every `ShardTable` update carries an epoch.
- Updates with stale epochs are silently ignored.
- `ShardMoved` control frames carry the new epoch so senders can correct their
  cached route before redelivery.
- Node-down events trigger `invalidate_for_node()` — epoch check is bypassed
  because the owning node is gone.

---

## 7. Observability

### Metrics (via existing `MpscRingBuffer`)

| Metric | Type | Description |
|--------|------|-------------|
| `hpactor_cluster_nodes{state}` | Gauge | Node count by state |
| `hpactor_cluster_state_transitions_total` | Counter | State transitions |
| `hpactor_cluster_quarantine_total` | Counter | Quarantine events |
| `hpactor_cluster_route_invalidations_total` | Counter | Route invalidation operations |
| `hpactor_shards_total` | Gauge | Total shard count |
| `hpactor_shard_moves_total` | Counter | Shard ownership transfers |
| `hpactor_shard_handoff_duration_seconds` | Histogram | Handoff latency |
| `hpactor_cluster_singleton_owner` | Gauge | 1 = this node owns the singleton |
| `hpactor_cluster_singleton_failover_total` | Counter | Singleton failover events |
| `hpactor_cluster_singleton_fencing_rejects_total` | Counter | Stale fencing token rejections |

### CLI Commands

| Command | Subsystem |
|---------|-----------|
| `/cluster nodes` | List all nodes with state, incarnation, epoch |
| `/cluster node <id> show` | Detailed node view with transition history |
| `/cluster shards` | Shard table with owners and epochs |
| `/cluster shard <id> show` | Single shard detail |
| `/cluster singletons` | Registered singletons with owner and state |
| `/cluster singleton <name> show` | Singleton detail with fencing token |
| `/cluster singleton <name> force-election` | Operator-forced re-election |

---

## 8. Dependency Graph

```
GossipMembership (SWIM, existing)
        │
        ▼
CLU-001 ClusterFailureModel ─────────────────────┐
        │                                         │
        ├──► RouteInvalidation                    │
        │     ├── ActorLocationCache::purge       │
        │     ├── ConnectionPool::mark_unavail    │
        │     └── OutboundTracker::fail_pending   │
        │                                         │
        ├──► CLU-002 Sharding                     │
        │     ├── ShardResolver (pure function)    │
        │     ├── ShardTable (local cache)         │
        │     ├── IPlacementStrategy               │
        │     │   ├── StaticPlacement              │
        │     │   └── RendezvousHash               │
        │     ├── ShardCoordinatorCore             │
        │     ├── ShardCoordinatorActor (singleton)│
        │     └── ShardHandoff (FSM)               │
        │                                         │
        └──► CLU-003 Singleton                    │
              ├── ISingletonElection               │
              │   └── OldestNodeElection            │
              ├── SingletonManagerCore             │
              └── SingletonManagerActor            │
                    │                              │
                    └── spawns ShardCoordinatorActor (first consumer)
```

**Future consumers of the singleton infrastructure:**
- `PubSubMediator` — distributed pub-sub topic registry
- `ClusterReceptionist` — cross-node `ServiceKey`-based actor discovery
- `LeaderElection` — pluggable consensus for coordinator roles

---

## 9. Acceptance Criteria Summary

1. **Node lifecycle:** All 8 states defined with constexpr-verified legal transitions.
   Detection events feed the failure model but don't directly mutate placement.

2. **Fencing:** Duplicate node identity triggers quarantine. Stale incarnation
   connections are fenced. Stale fencing tokens are rejected.

3. **Sharding:** Logical actor IDs map deterministically to shards. Shard ownership
   changes invalidate stale `ActorLocationCache` entries. Node loss triggers shard
   reassignment. Handoff drain/recover cycle is observable.

4. **Singleton:** At most one active owner per singleton identity. Conflicting
   owners are fenced via token rejection. Failover occurs on owner node `Down`.
   `ShardCoordinatorActor` validates the entire singleton infrastructure end-to-end.

5. **Route invalidation:** Consistent across `ActorLocationCache`, `ActorProxy`,
   `RpcChannel`, remote spawn, `ConnectionPool`, and `OutboundTracker`.

6. **Opt-in cluster mode:** Single-node applications are unaffected. `enable_cluster()`
   is the only entry point.

7. **Deterministic tests:** All tests pass without real threads, wall-clock timing,
   or network I/O. 98 cluster-specific tests across 11 test suites.

---

## 10. Future Evolution

Items designed but deferred to future sprints:

| Capability | Status | Design Doc |
|-----------|--------|------------|
| Distributed Pub-Sub | Design complete | `docs/architecture/production/distributed-pub-sub-design.md` |
| Cluster Receptionist | Design complete | `docs/architecture/production/cluster-receptionist-design.md` |
| Leader Election (Raft/etcd) | Interface defined | `docs/architecture/production/leader-election-design.md` |
| Load-Aware Placement | Interface slot exists | Placement strategy plug-in |
| Multi-Zone Placement | Not started | CLU-004 |
| External Coordinator (etcd/Consul) | Interface slot exists | `ISingletonElection` plug-in |
| Admin API (REST/gRPC) | Not started | OPS-002 |
| Dynamic Config Reload | Not started | OPS-003 |

---

## 11. References

- [Production Reliability Plane](../production/production-reliability-plane.md) — top-level 24x7 roadmap
- [Cluster Failure Model Design](../production/cluster-failure-model-design.md) — detailed CLU-001 design
- [Cluster Sharding & Placement Design](../production/cluster-sharding-placement-design.md) — detailed CLU-002 design
- [Feature Gap Refined Requirement Backlog](../production/feature-gap-refined-requirement-backlog.md) — CLU-003, MSG-003 cards
- [Akka Gap Analysis (Issue #329)](https://github.com/skg7on/HPActor/issues/329) — HPActor vs. Akka Typed comparison
- Sprint 2 Design Spec: `docs/superpowers/specs/2026-06-21-akka-gap-closure-sprint2-design.md`
- Sprint 3 Design Spec: `docs/superpowers/specs/2026-06-22-akka-gap-closure-sprint3-design.md`
- Sprint 2 Implementation Plan: `docs/superpowers/plans/2026-06-21-akka-gap-closure-sprint2-impl.md`
- Sprint 3 Implementation Plan: `docs/superpowers/plans/2026-06-22-akka-gap-closure-sprint3-impl.md`
