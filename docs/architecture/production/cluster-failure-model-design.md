# Cluster Failure Model Architecture Design

## 1. Executive Summary

HPActor has service discovery and SWIM-style gossip membership, but production
clusters need a higher-level failure model that defines what node states mean,
how partitions are handled, when actor routes are invalidated, and how the
runtime prevents unsafe split-brain behavior.

This design separates membership detection from cluster decisions. Gossip can
suspect and mark nodes dead, but a cluster failure model decides route
invalidation, quarantine, fencing, shard ownership changes, and recovery.

## 2. Goals

1. Define stable node states and legal transitions.
2. Separate detection from control-plane action.
3. Handle partitions, flapping nodes, and partial connectivity.
4. Invalidate actor routes safely when nodes fail.
5. Support future sharding, placement, and reliable messaging.
6. Expose cluster health and failure decisions to operators.

## 3. Non-Goals

- Implementing consensus in the first version.
- Guaranteeing availability and consistency at the same time during partitions.
- Hiding all network failures from user actors.

## 4. Node State Model

```cpp
enum class ClusterNodeState : uint8_t {
    Joining,
    Alive,
    Suspect,
    Unreachable,
    Quarantined,
    Leaving,
    Down,
    Removed,
};
```

State meanings:

- `Joining`: node discovered, not yet trusted for placement.
- `Alive`: node can receive new work.
- `Suspect`: failure detector is probing the node.
- `Unreachable`: node cannot be contacted from this node's perspective.
- `Quarantined`: node identity or epoch conflict makes communication unsafe.
- `Leaving`: graceful shutdown has begun.
- `Down`: node considered failed for routing and placement.
- `Removed`: node is no longer part of cluster metadata.

## 5. Cluster Epochs And Fencing

Each node should advertise:

- Stable node id.
- Incarnation number.
- Cluster id.
- Membership epoch.
- Process start id.
- Transport identity fingerprint.

Fencing rules:

- If a node restarts with the same node id but a higher incarnation, older
  connections are fenced.
- If two live processes claim the same node id and incarnation, both are
  quarantined until operator action or a deterministic tie-breaker resolves it.
- A node from a different cluster id is rejected even if discovery sees it.

## 6. Partition Policy

Partition behavior must be configurable by workload.

Recommended policies:

- `FailClosed`: stop remote delivery and shard movement unless quorum is known.
- `FailOpen`: continue best-effort messaging to reachable nodes.
- `StaticMajority`: only a configured majority partition can own singleton or
  shard-coordinator roles.
- `ExternalCoordinator`: delegate ownership decisions to etcd, Consul, or a
  future consensus adapter.

Default policy for first implementation: `FailOpen` for ordinary best-effort
actor messages, `FailClosed` for cluster singleton and shard ownership.

## 7. Route Invalidation

When a node enters `Down`, `Removed`, or `Quarantined`:

- Purge `ActorLocationCache` entries pointing to that node.
- Fail pending remote spawn and RPC requests.
- Mark outbound connection pool endpoints unavailable.
- Dead-letter in-flight messages that cannot be retried.
- Notify linked and monitored actors with a structured remote-down event.

## 8. Recovery Rules

When a node returns:

- It starts as `Joining`.
- It must advertise a fresh incarnation and process start id.
- Old actor locations are not trusted.
- Shard ownership and actor placement are recalculated by the control plane.
- Durable actors must recover state before advertising readiness.

## 9. Observability

Metrics:

- `hpactor_cluster_nodes`
- `hpactor_cluster_state_transitions_total`
- `hpactor_cluster_suspect_total`
- `hpactor_cluster_quarantine_total`
- `hpactor_cluster_route_invalidations_total`

Logs:

- State transition with old state, new state, reason, incarnation, and detector.
- Quarantine decision with conflict fields.
- Partition policy decision.

CLI:

- `/cluster nodes`
- `/cluster node <id> show`
- `/cluster partitions`
- `/cluster quarantine`

## 10. Acceptance Criteria

- Node states and transitions are explicit and testable.
- Detection events do not directly mutate actor placement without policy.
- Route invalidation is consistent across ActorProxy, location cache, RPC, and
  remote spawn.
- Quarantine prevents ambiguous node identity from receiving production traffic.
- Operators can inspect why a node changed state.

