# Graceful Shutdown and Rolling Upgrade Architecture Design

## 1. Executive Summary

Production actors must stop safely. A node should drain work, reject or reroute
new messages, complete critical in-flight operations, leave the cluster, and
release resources without corrupting state or surprising upstream producers.

This design defines shutdown phases, drain protocol, actor stop behavior,
cluster leave behavior, and rolling upgrade compatibility.

## 2. Goals

1. Provide deterministic node shutdown phases.
2. Allow actors to drain or fail fast by policy.
3. Stop accepting new work before terminating actors.
4. Preserve system messages needed for shutdown.
5. Support rolling upgrades with mixed protocol versions.
6. Expose shutdown progress to health checks and operators.

## 3. Non-Goals

- Infinite wait for stuck actors.
- Transparent live code replacement inside an actor.
- Cross-version compatibility without a declared protocol range.

## 4. Node Shutdown Phases

```cpp
enum class ShutdownPhase : uint8_t {
    Running,
    DrainingIngress,
    DrainingActors,
    LeavingCluster,
    FlushingTelemetry,
    Stopped,
    ForcedStop,
};
```

Phase behavior:

- `DrainingIngress`: HTTP gateways, remote spawn, and admin mutating commands
  stop accepting new user work.
- `DrainingActors`: actor mailboxes drain according to per-actor policy.
- `LeavingCluster`: discovery advertises `Leaving`, placement moves shards.
- `FlushingTelemetry`: logs, metrics, traces, and DLQ export complete best
  effort.
- `ForcedStop`: timeout exceeded; runtime terminates remaining actors.

## 5. Actor Drain Policy

Per actor:

- `Drain`: process existing mailbox before stop.
- `DropUserMessages`: dead-letter user messages, keep system messages.
- `SnapshotAndStop`: durable actors persist state and stop.
- `ImmediateStop`: stop actor immediately.
- `TransferShard`: sharded actors hand off ownership before stop.

System messages required for shutdown bypass normal user-message rejection.

## 6. Shutdown API

```cpp
struct ShutdownOptions {
    uint32_t ingress_timeout_ms{5000};
    uint32_t actor_drain_timeout_ms{30000};
    uint32_t cluster_leave_timeout_ms{10000};
    bool force_after_timeout{true};
};

result<void> ActorSystem::shutdown(const ShutdownOptions& options);
```

CLI:

```text
/system drain
/system shutdown --timeout 30s
/system shutdown --force
/system shutdown status
```

## 7. Rolling Upgrade Model

Rolling upgrade requires:

- Protocol version negotiation on transport handshake.
- Feature flags in node membership metadata.
- Backward-compatible frame decoding.
- Config schema versioning.
- Placement drain before process stop.
- Health readiness false before leaving starts.

Upgrade states:

- `Ready`
- `Draining`
- `Leaving`
- `Restarting`
- `Joining`
- `Ready`

## 8. Compatibility Rules

Each node advertises:

- runtime version
- protocol min/max version
- enabled feature flags
- config schema version
- tracing/mailbox/reliable messaging capability flags

If peers do not share a protocol range, they do not exchange actor traffic and
the newer node reports incompatibility.

## 9. Health Integration

Health endpoints:

- Liveness remains true until process cannot make progress.
- Readiness becomes false at `DrainingIngress`.
- Startup remains false until topology loaded, discovery started, and required
  system actors are ready.

## 10. Observability

Metrics:

- `hpactor_shutdown_phase`
- `hpactor_shutdown_duration_seconds`
- `hpactor_shutdown_actor_drain_pending`
- `hpactor_shutdown_forced_total`

Logs:

- Phase transition.
- Actor drain timeout.
- Shard handoff timeout.
- Forced stop reason.

## 11. Acceptance Criteria

- Shutdown enters readiness false before rejecting new ingress.
- Actor drain policy is configurable.
- Stuck actors cannot block shutdown forever.
- Cluster membership advertises leaving before process stop.
- Mixed-version nodes negotiate protocol compatibility.
- Operators can inspect shutdown progress.

