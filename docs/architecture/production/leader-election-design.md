# Leader Election for Cluster Singletons — Design Document

## 1. Executive Summary

The HPActor cluster singleton subsystem requires a pluggable leader election mechanism to determine which node owns a given singleton across the cluster. The `ISingletonElection` interface (defined in Sprint 2) provides the abstraction point. Sprint 4 adds two concrete election strategies beyond the existing `OldestNodeElection`:

- **`MajorityBasedElection`** — Requires a strict majority (> N/2) of alive nodes to vote for a candidate before declaring a leader. Provides safety during network partitions.
- **`FixedPriorityElection`** — Static per-node priority where the highest-priority alive node wins. Useful for controlled failover scenarios (e.g., primary data center preference).

## 2. Architecture

### 2.1 Interface (existing from Sprint 2)

```
ISingletonElection (abstract)
├── elect(SingletonIdentity, span<alive_nodes>) → optional<NodeId>
├── on_peer_down(node_id)
│
├── OldestNodeElection          (Sprint 2) — lowest node_id wins
├── MajorityBasedElection       (Sprint 4) — > N/2 vote threshold
└── FixedPriorityElection       (Sprint 4) — configurable static priority
```

### 2.2 MajorityBasedElection

**Algorithm:**
1. Votes are accumulated via `record_vote(singleton_name, voter, voted_for)` — called by `SingletonManagerActor` when gossip piggyback carries vote entries.
2. `elect(id, alive_nodes)` tallies live votes per candidate:
   - Excludes votes from nodes not in `alive_nodes` (dead voters).
   - Excludes candidates not in `alive_nodes` (dead candidates).
3. If any candidate has ≥ `(alive_nodes.size() / 2) + 1` votes, returns that candidate.
4. Otherwise returns `nullopt` (no leader).

**Safety properties:**
- **At most one leader:** Two candidates cannot both reach > N/2 threshold simultaneously.
- **Partition-safe:** During a network split with even node distribution, no leader is elected (fail-closed).
- **Liveness:** A single candidate with all alive nodes voting for it will be elected.

**Gossip integration:**
- Each node includes its vote `(singleton_name, voted_node_id)` in gossip piggyback metadata.
- On receive, unpacks vote entries and calls `record_vote()`.
- `on_peer_down(node_id)` removes votes from the dead node and clears the dead node as a candidate.

### 2.3 FixedPriorityElection

**Algorithm:**
1. Constructor takes a `unordered_map<node_id, priority>` where higher values = preferred owner.
2. `elect(id, alive_nodes)` finds the alive node with the highest priority.
3. Ties are broken by lowest `node_id` (lexicographic).
4. Nodes without a priority entry are excluded from consideration.

**Use cases:**
- Primary data center preference (zone-A nodes at priority 100, zone-B at priority 50).
- Controlled failover testing (manually configure which node should be owner).
- Operator-forced leader (set a single node to max priority).

## 3. Integration Points

### 3.1 With SingletonManagerCore
- `SingletonManagerCore` holds an `ISingletonElection*` (or `unique_ptr`).
- On node state changes (from `ClusterFailureModel` observer callback), calls `elect()`.
- If election returns a different owner, triggers singleton state transition (Active → Draining → Standby or Standby → Activating → Active).

### 3.2 With GossipMembership (MajorityBasedElection only)
- `SingletonManagerActor` encodes votes in gossip piggyback metadata entries.
- On receiving gossip piggyback, decodes vote entries and calls `MajorityBasedElection::record_vote()`.
- Uses existing `PiggybackType::Metadata` channel — no new gossip protocol needed.

## 4. Configuration (TOML)

```toml
[system.cluster.singleton]
# Election strategy: "oldest", "majority", or "fixed"
election_strategy = "majority"

[system.cluster.singleton.fixed_priority]
"node-primary" = 100
"node-secondary" = 50
"node-tertiary" = 10
```

## 5. Testing

### 5.1 MajorityBasedElection (11 tests)
- Solo node has majority (1/1)
- Two of three is majority (2/3)
- Three of five is majority (3/5)
- No majority returns nullopt (1 vote each of 3)
- Tie with two nodes no majority (1/2)
- Empty alive nodes returns nullopt
- No votes recorded returns nullopt
- Node removal flips majority
- OnPeerDown clears votes from dead node
- RecordVote overwrites previous vote
- Different singleton names are independent

### 5.2 FixedPriorityElection (8 tests)
- Highest priority wins
- Only alive nodes considered
- Tie broken by lowest node id
- Empty alive nodes returns nullopt
- Nodes not in priority map are excluded
- OnPeerDown records known dead
- Deterministic same input same output
- Single node with priority wins

## 6. Design Decisions

1. **Not Raft/Paxos:** `MajorityBasedElection` uses gossip-accumulated votes, not a formal consensus protocol. This trades crash-fault tolerance for implementation simplicity. The `ISingletonElection` interface is pluggable for future Raft/etcd backends.

2. **`record_vote()` is concrete-class API, not interface:** Different election strategies have different input mechanisms. `OldestNodeElection` needs no votes. `MajorityBasedElection` needs votes. `FixedPriorityElection` needs priorities. The common interface is `elect()` + `on_peer_down()`.

3. **No persistent vote storage:** Votes are in-memory only. Node restart resets vote state. This is acceptable because `SingletonManagerCore` already handles incarnation-based fencing — a restarted node will not claim ownership if its incarnation is stale.

## 7. References
- [Cluster Subsystem Architecture](../cluster/cluster-subsystem-architecture.md) — Section 4.3 Cluster Singleton
- [Feature Gap Refined Requirement Backlog](feature-gap-refined-requirement-backlog.md) — CLU-003
- [Akka Gap Analysis (Issue #329)](https://github.com/skg7on/HPActor/issues/329)
- Sprint 2: CLU-003 types (PR #347)
- Sprint 3: CLU-003 actor integration (PR #348)
