# ActorSystem Phase 7 Observability and Cluster Boundary Design

**Date:** 2026-06-28

**Status:** Proposed phase design

**Parent design:**
`docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`

**Prerequisites:** Phases 0–6 are merged. Runtime construction uses an
immutable blueprint; `RuntimeCoordinator` supplies one ordered start/stop path;
actor, messaging, stream, router, and network owners expose bounded snapshots
and stable ports.

**Scope:** Move metrics, logging, and tracing resource ownership into an
`ObservabilityRuntime`; replace `unique_ptr<void>` cluster ownership and casts
with `IClusterRuntime`; and make CLI/admin/health inspection consume read-only
component snapshots through an ownership-free operations view. Preserve public
telemetry and cluster accessors as compatibility adapters for Phase 8.

## 1. Summary

Phase 7 finishes the remaining cross-cutting ownership seams:

```text
RuntimeCoordinator
  +-- ObservabilityRuntime
  |     +-- LogManager / Logger sink
  |     +-- TraceManager / exporter
  |     +-- Metrics ring / aggregator / exporters
  |     +-- prepare/commit reload + flush snapshot
  |
  +-- optional IClusterRuntime
  |     +-- failure model
  |     +-- singleton coordination
  |     +-- route invalidation
  |     +-- typed node-event ingress
  |
  +-- OperationsSnapshotService (orchestration without ownership)
        +-- copies lifecycle/actor/messaging/stream/network/
            observability/cluster snapshots
        +-- CLI/admin/health render copied values only
```

`ObservabilityRuntime` owns telemetry infrastructure. It starts before every
producer and stops after every producer; flush occurs while sinks/exporters are
alive. Actor objects used to aggregate metrics remain actor-owned: the
`MetricsActor` instance, context, mailbox, and directory entry belong to
`ActorRuntime`, while `ObservabilityRuntime` owns the ring/config/exporter and
requests installation through a system-actor port.

`IClusterRuntime` is a real cross-library control-plane boundary. Core code
owns `std::unique_ptr<IClusterRuntime>` and never stores `void*`, cleanup
function pointers, or downcasts. Concrete cluster construction is supplied by
a typed factory so the core/cluster link boundary remains explicit.

## 2. Current-State Evidence

Current `ActorSystem` directly owns metrics config/ring/raw metrics actor,
logging config/manager/raw logger, tracing config/manager, and three cluster
objects held as `unique_ptr<void, cleanup_fn>`.

Telemetry is started in the constructor and stopped in the destructor around
independently ordered scheduler/network teardown. Producers receive raw ring or
logger pointers through late setters. `apply_tracing_config()` can destroy and
replace the trace manager while active producers may retain a facade-derived
target.

`enable_cluster()` allocates concrete failure-model, singleton-manager, and
route-invalidation objects, stores them type-erased, then repeatedly
`static_cast`s them in observers and public accessors. This hides dependency
and destruction order from the type system.

CLI/admin commands commonly reach through `ActorSystem` to mutable subsystem
objects. Some actor traversal helpers document that callbacks execute while a
registry lock is held, making formatting or future I/O inside inspection paths
a lock-order and latency risk.

## 3. Important Correctness Findings

### 3.1 Telemetry producer pointers can outlive or race their sinks

Scheduler, delivery, backpressure, and transport receive raw metrics/logger
pointers. Current startup/reload/shutdown ordering does not centrally prove
that the sink remains alive until all producers quiesce.

Required contract:

- `ObservabilityRuntime` is constructed before producers and destroyed after
  them;
- stable producer ports keep the same address for the runtime lifetime;
- disabling/reconfiguring changes internal policy, not the port object;
- all producer threads stop before final flush and sink destruction; and
- late events after the close gate are boundedly counted/dropped, never sent
  to freed memory.

### 3.2 Metrics actor ownership is currently split

The facade owns the ring and a raw pointer to an actor whose real ownership is
in actor structures. Treating the actor as observability-owned would recreate
dual ownership and bypass actor adoption/stop rules.

Required contract: `ActorRuntime` exclusively owns the actor object, mailbox,
context, and directory entry. `ObservabilityRuntime` owns metrics storage and
requests/revokes the system actor using a narrow port. Snapshots identify the
actor by stable address/health value, not an owning/raw escape.

### 3.3 Trace reload replaces an active manager without producer quiescence

`apply_tracing_config()` stops/resets/creates/starts a manager directly. A
producer can race the replacement or lose/duplicate spans.

Required contract: producers write through one stable `TraceSinkPort`.
Supported live reload prepares an immutable sampler/exporter policy, atomically
publishes it, drains the retired exporter, and reports loss/timeout. If the
manager cannot meet that contract, the relevant field is restart-required.

### 3.4 Telemetry flush order can lose final runtime evidence

Stopping logs/traces before scheduler/network producers or destroying metrics
storage without a final producer barrier can discard the events most useful
during shutdown incidents.

Required order:

1. readiness false and ingress closed;
2. all network/actor/scheduler producers quiesced;
3. metrics aggregation drains to its boundary;
4. trace exporters flush within deadline;
5. log manager flushes final lifecycle records;
6. stable ports close and managers are destroyed.

Flush failures are reported in the lifecycle snapshot but do not resurrect
producers or block teardown indefinitely.

### 3.5 `unique_ptr<void>` erases cluster invariants

Type-erased ownership allows mismatched deleters/casts, obscures optional
dependencies, and prevents the coordinator from calling a typed lifecycle
contract.

Required contract: core holds `std::unique_ptr<IClusterRuntime>`. The interface
defines start/stop, node events, snapshot, readiness/health contribution, and
explicit legacy typed views where compatibility truly requires them. No
`void*`, cleanup function pointer, `static_cast` from void, `dynamic_cast`, or
RTTI is used.

### 3.6 Cluster observers capture facade state and can re-enter teardown

Failure-model observers capture `ActorSystem` and call other type-erased
objects. A node event can arrive from discovery while cluster teardown or
singleton election is active.

Required contract: `NetworkRuntime` sends node changes to a stable
`ClusterNodeEventSink`. `IClusterRuntime` serializes its control-plane state,
closes its event gate before stop, drains callbacks, and never calls through
the facade. Route invalidation and singleton coordination are internal cluster
implementation details.

### 3.7 CLI/admin inspection can hold subsystem locks across rendering/I/O

Inspection that iterates live owners through callbacks can hold actor/stream/
cluster locks while allocating strings, formatting tables, or writing sockets.

Required contract: every component produces a bounded value snapshot by
copying under its own lock and releasing it before aggregation/rendering.
`OperationsSnapshotService` never holds two component locks simultaneously and
owns none of the inspected state.

### 3.8 An aggregate snapshot is not instantaneously atomic

Component snapshots are copied sequentially, so their epochs can differ.
Claiming a globally atomic view would be incorrect and could mislead incident
diagnosis.

Required contract: the aggregate carries collection start/end time, lifecycle
epoch, and each component's own epoch. CLI/admin labels it a coordinated
best-effort snapshot. Strict point-in-time consistency is not promised.

## 4. Goals and Non-Goals

### 4.1 Goals

- One telemetry infrastructure owner and stable producer ports.
- Lifecycle-ordered telemetry start, producer quiescence, and final flush.
- Typed optional cluster runtime with no core type erasure/downcast.
- Snapshot-only operations surfaces with bounded lock scope.
- Equivalent or better metrics, logs, traces, CLI, admin, and health detail.
- Compatibility forwards for existing public accessors.

### 4.2 Non-goals

- Replacing existing telemetry formats/vendors or cluster algorithms.
- Making operations snapshots globally atomic.
- Moving actor object ownership into observability or cluster components.
- Distributed consensus for reload or shutdown.
- Removing public compatibility APIs or finishing PImpl (Phase 8).
- Adding a generic event bus/service locator or virtual dispatch to message
  hot paths.

## 5. Target Architecture

### 5.1 `ObservabilityRuntime`

Private component API:

```cpp
class ObservabilityRuntime final {
public:
    result<void> start() noexcept;
    result<void> stop(ObservabilityStopRequest) noexcept;
    result<PreparedObservabilityReload> prepare_reload(
        const ObservabilityConfig&) noexcept;
    result<void> commit_reload(PreparedObservabilityReload&&) noexcept;

    MetricsSinkPort metrics_sink() noexcept;
    LogSinkPort log_sink() noexcept;
    TraceSinkPort trace_sink() noexcept;
    ObservabilitySnapshot snapshot() const noexcept;
};
```

Ports are stable objects owned by the runtime even when a feature is disabled.
Disabled ports are bounded no-ops with counters; callers never branch on a raw
manager lifetime. This is intentionally different from disabled
`NetworkRuntime`: telemetry ports are ubiquitous hot-path dependencies whose
stable identity prevents dangling pointers.

### 5.2 Metrics ownership

`ObservabilityRuntime` owns metrics config, ring/storage, aggregation state,
exporters, and port. `ActorRuntime` owns the `MetricsActor`. A fixed
`TelemetrySystemActorPort` installs/removes it using canonical Phase 2
adoption. The actor receives a shared/stable metrics data handle whose lifetime
is subordinate to the coordinator order, not a facade pointer.

### 5.3 Logging and tracing ownership

Log/trace managers and exporter threads belong to `ObservabilityRuntime`.
Producers receive small stable ports. Reload updates internal immutable policy
through the Phase 6 transaction. Raw manager/logger accessors are compatibility
views only and are never used by new core code.

### 5.4 `IClusterRuntime`

Public core-facing interface:

```cpp
class IClusterRuntime {
public:
    virtual ~IClusterRuntime() = default;
    virtual result<void> start() noexcept = 0;
    virtual result<void> stop(ClusterStopRequest) noexcept = 0;
    virtual void on_member_changed(const net::Member&, bool) noexcept = 0;
    virtual ClusterSnapshot snapshot() const noexcept = 0;
    virtual ClusterLegacyViews legacy_views() noexcept = 0;
};
```

Virtual dispatch is acceptable on this low-frequency cross-library control
plane. Actor delivery and frame hot paths do not use it. `ClusterLegacyViews`
contains explicitly typed nullable pointers only for existing accessors; it is
deprecated in Phase 8 and cannot confer ownership.

### 5.5 Cluster factory and ownership

The cluster library supplies a typed factory registration/injection:

```cpp
using ClusterRuntimeFactory = result<std::unique_ptr<IClusterRuntime>> (*)(
    const ClusterRuntimeConfig&, ClusterRuntimeDependencies) noexcept;
```

`RuntimeBuilder` invokes it only when cluster is enabled. Missing factory is a
typed startup validation error. Core owns the returned interface. Concrete
failure model, singleton, and route invalidation are private to the cluster
implementation and destroyed by its normal virtual destructor.

### 5.6 Operations snapshot service

`OperationsSnapshotService` contains non-owning fixed snapshot ports for the
coordinator and components. It copies each snapshot in a fixed order, never
holds two component locks, and then returns one bounded aggregate. CLI/admin/
health handlers format or serialize only the returned values.

Large actor/stream listings use bounded pagination tokens and per-component
limits. Snapshot creation does not call user actors, transport, exporters, or
formatters under component locks.

### 5.7 Lifecycle order

Startup:

1. construct stable observability ports/managers;
2. start log/metrics/trace infrastructure;
3. construct/start all producers and optional cluster runtime;
4. install metrics system actor and operations snapshot service;
5. reach normal Phase 6 readiness.

Shutdown:

1. readiness false, network/cluster ingress gates close;
2. cluster events drain and cluster stops;
3. network/actors/scheduler and all telemetry producers stop;
4. metrics actor is removed through `ActorRuntime`;
5. observability drains, flushes, closes ports, and stops;
6. operations view and component graph are destroyed.

## 6. Concurrency and Resource Contract

- Metrics/log/trace producer ports are thread-safe, bounded, and stable.
- Port calls after close are counted/dropped without dereferencing retired
  manager state.
- Exporters never block cooperative scheduler/event-loop threads; existing
  bounded queues/background mechanisms remain authoritative.
- Cluster state changes are serialized by the cluster implementation and
  callbacks are quiesced before stop returns.
- Snapshot functions have documented maximum copied entries/bytes and never
  call external code under a component lock.
- Aggregate snapshots carry independent component epochs.
- Telemetry memory accounting, ring bounds, drop policy, and trace/log back
  pressure are preserved and exposed.

## 7. Compatibility Strategy

- `metrics_ring_buffer()`, `metrics_actor()`, `trace_manager()`,
  `log_manager()`, and logger accessors forward to compatibility views.
- `enable_cluster()` becomes a compatibility request routed through the
  coordinator/factory and returns/records typed failure according to its
  existing signature constraints.
- `cluster_failure_model()`, `singleton_manager()`, and
  `route_invalidation()` use `ClusterLegacyViews`; core performs no cast.
- New code uses ports/snapshots and cannot include concrete manager/cluster
  implementation headers through `ActorSystem`.
- Phase 8 marks unsafe raw views deprecated and removes internal adapters after
  call-site migration.

## 8. Operations and Failure Semantics

Snapshots expose queue capacity/use/drops, exporter status/last error/flush
age, trace sampling state, cluster state/quorum/member count/singleton state,
component epochs, and lifecycle state. Sensitive payloads, TLS material, and
unbounded log text are excluded.

Telemetry startup failures follow blueprint policy: required sink failure
fails startup; optional exporter failure produces a typed degraded state only
when explicitly configured. Cluster failure makes readiness false when cluster
mode is required. Flush uses a deadline and records incomplete counts.

## 9. Migration Sequence

1. Inventory all telemetry producers, raw setters, actor ownership, cluster
   casts, and CLI/admin direct traversals.
2. Add stable telemetry ports and fake-backed observability lifecycle tests.
3. Move metrics storage/exporters and install metrics actor through actor port.
4. Move log and trace managers, then convert producers to stable ports.
5. Integrate coordinator start/quiesce/flush/stop and live reload.
6. Add `IClusterRuntime`, typed factory, and concrete aggregate implementation.
7. Route node events and compatibility cluster accessors through the interface.
8. Add component snapshots/operations aggregation and migrate CLI/admin/health.
9. Delete facade owner fields, raw internal setters, `unique_ptr<void>`, casts,
   and lock-held rendering paths.

## 10. Testing Strategy

- Telemetry port identity across enable/disable/reload and after close.
- Producer/quiescence/flush ordering, full queues, optional exporter failure,
  reload prepare/commit/retirement, and final-event preservation.
- Metrics actor canonical adoption/removal and no dual ownership.
- Cluster factory absent/failure/disabled/enabled, node event/stop race,
  singleton/invalidation behavior, and typed legacy views.
- Snapshot bounds/pagination/epochs, concurrent mutation, and no lock-held
  formatter/I/O callback.
- CLI/admin/health output parity plus new lifecycle/degraded information.
- ASan repeated telemetry/cluster fail-stop-destroy; TSAN producer/reload/flush,
  node event/stop, and snapshot mutation tests.

## 11. Acceptance Criteria

1. One `ObservabilityRuntime` owns telemetry infrastructure.
2. Stable ports outlive every producer; flush occurs after producer quiescence.
3. ActorRuntime exclusively owns the metrics actor object/adoption state.
4. Core has no `unique_ptr<void>` cluster owner or void/downcast cleanup.
5. Optional cluster runtime uses a typed interface and factory.
6. Network node events do not capture or call through `ActorSystem`.
7. CLI/admin/health consume copied bounded snapshots only.
8. No operations formatter/I/O runs under component locks.
9. Existing public telemetry/cluster APIs compile through adapters.
10. Focused normal, architecture, ASan, and TSAN evidence passes.

## 12. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Stable no-op ports add hot-path cost | Fixed branch/atomic pointer, benchmark against current setters |
| Metrics actor appears observability-owned | Explicit system-actor port; actor resources remain in ActorRuntime |
| Cluster interface grows into a service locator | Lifecycle, node events, snapshot, and temporary legacy views only |
| Aggregate snapshot appears atomic | Carry per-component epochs and collection interval |
| Flush blocks shutdown indefinitely | Deadline, bounded queues, incomplete-flush report |
| Legacy raw accessors bypass new rules | Compatibility-only views; migrate internal users and deprecate in Phase 8 |

## 13. Decision Summary

- Own telemetry infrastructure in one lifecycle component.
- Keep actor object ownership in `ActorRuntime`.
- Give producers stable bounded ports rather than replaceable raw managers.
- Use a typed virtual cluster boundary only on the control plane.
- Use snapshots as the operations contract; aggregate without ownership.
- Preserve public raw access temporarily, but prohibit new internal use.
