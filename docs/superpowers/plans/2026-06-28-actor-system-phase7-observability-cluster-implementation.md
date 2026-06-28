# ActorSystem Phase 7 Observability and Cluster Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` or `superpowers:executing-plans`.
> Invoke `.claude/skills/tddflow-development/` before production edits and
> `superpowers:verification-before-completion` before commits/completion.

**Goal:** Establish one telemetry infrastructure owner, a typed optional
cluster boundary, and snapshot-only operations inspection while preserving
public compatibility.

**Architecture:** `ObservabilityRuntime` owns metrics/log/trace resources and
stable producer ports; `ActorRuntime` retains metrics-actor ownership;
`IClusterRuntime` replaces type-erased cluster fields; an ownership-free
`OperationsSnapshotService` copies bounded component snapshots for CLI/admin/
health.

**Tech Stack:** C++20, typed fixed ports, one low-frequency virtual cluster
interface, CMake/Ninja, GoogleTest, CTest architecture checks, ASan/TSAN, no
RTTI/exceptions.

## Global Constraints

- Start after Phase 6 is merged with lifecycle/fault-matrix evidence.
- Work in `.claude/worktrees/actor-system-observability-cluster/` on branch
  `refactor/actor-system-observability-cluster` from updated `origin/main`.
- Verify path/branch before writes; RED -> GREEN -> REFACTOR.
- Observability infrastructure starts before and stops after every producer.
- ActorRuntime owns every actor object/mailbox/context/directory entry.
- Stable producer port identity must not change during reload or disable.
- The cluster virtual boundary is control-plane only; no message/frame hot-path
  virtual dispatch.
- Operations snapshotting never holds two component locks and never formats,
  logs, writes sockets, calls actors, or invokes user code under a lock.
- Preserve public telemetry/cluster signatures as adapters until Phase 8.
- Add no service locator, generic event bus, `void*` owner, downcast, RTTI, or
  exception flow.

## Expected File Structure

**Create:**

- `src/runtime/observability_runtime.hpp/.cpp`
- `src/runtime/observability_ports.hpp`
- `src/runtime/operations_snapshot_service.hpp/.cpp`
- `include/hpactor/observability/observability_snapshot.hpp`
- `include/hpactor/runtime/operations_snapshot.hpp`
- `include/hpactor/cluster/cluster_runtime.hpp`
- concrete cluster runtime/factory bridge in `src/cluster/`
- unit tests for observability, cluster interface, snapshot service
- integration tests for flush order, cluster lifecycle, operations surfaces
- architecture checks for telemetry/cluster/inspection boundaries

**Modify:** runtime builder/coordinator/Impl; metrics/log/tracing producers and
managers; cluster bridge/CMake; facade accessors; network node event port;
CLI/admin/health commands; docs and `CLAUDE_MEMORY.md`.

---

### Task 0: Establish worktree and ownership/producer inventory

- [ ] Create/verify the prescribed worktree and branch.
- [ ] Verify Phase 6 blueprint, builder, coordinator, reload, and snapshots.
- [ ] Read rules, production plane, concurrency rules, Phase 7 design, and
  existing telemetry/cluster docs.
- [ ] Build/run metrics/log/trace/cluster/CLI/admin/health baselines.
- [ ] Inventory every telemetry producer, raw setter/accessor, callback thread,
  queue bound, start/stop/flush action, metrics actor owner, cluster allocation/
  cast, node-event route, and lock-held inspection callback.
- [ ] Add characterization tests for final shutdown events, trace reload,
  metrics actor ownership, cluster enabled/accessors, and operations output.
- [ ] Commit as `test: characterize observability and cluster ownership`.

---

### Task 1: Add stable telemetry ports and observability skeleton

- [ ] RED-test stable port address through disabled/enabled/reload/stop states,
  bounded no-op drops, concurrent producer calls, state transitions, and
  snapshot bounds.
- [ ] Define fixed `MetricsSinkPort`, `LogSinkPort`, and `TraceSinkPort` whose
  outer identity is lifetime-stable.
- [ ] Add immutable internal target publication/close gate with explicit memory
  ordering and bounded drop counters.
- [ ] Implement side-effect-free `ObservabilityRuntime` construction and
  coordinator-compatible start/stop/prepare-reload skeleton.
- [ ] Benchmark disabled/enabled port overhead against current raw setters.
- [ ] Commit as `refactor: add stable observability runtime ports`.

---

### Task 2: Move metrics infrastructure without moving actor ownership

- [ ] RED-test ring/storage/exporter owner, capacity/drop policy, producer
  wiring, canonical metrics actor adoption/removal, failed actor install
  rollback, drain, and no raw actor ownership.
- [ ] Move metrics config, ring/storage, aggregation/exporter state into
  `ObservabilityRuntime`.
- [ ] Implement `TelemetrySystemActorPort` in `ActorRuntime`; install/remove the
  metrics actor through Phase 2 canonical adoption.
- [ ] Replace scheduler/delivery/backpressure/transport raw ring setters with
  stable metrics port injection. Time-box compatibility setters for Phase 8.
- [ ] Keep ring address stable or hide it entirely behind the port; live reload
  must not invalidate producer state.
- [ ] Run metrics/actor/delivery/net tests and commit as
  `refactor: move metrics ownership into ObservabilityRuntime`.

---

### Task 3: Move logging and tracing with safe reload

- [ ] RED-test start failure, producer-before-manager prevention, live log
  policy reload, trace sampler/exporter prepare/commit, retired exporter drain,
  full queue, late event, flush deadline, and final lifecycle record.
- [ ] Move log/trace configs/managers/exporter threads into the component.
- [ ] Replace core raw logger/manager dependencies with stable ports.
- [ ] Implement Phase 6 reload transaction only for policies that can prepare,
  atomically publish, and drain/rollback. Reclassify unsupported fields as
  restart-required.
- [ ] Delete facade `apply_tracing_config()` and independent manager starts/
  stops; retain raw public compatibility views only.
- [ ] Run logging/tracing/reload tests and commit as
  `refactor: move logging and tracing ownership`.

---

### Task 4: Integrate telemetry lifecycle and prove final flush order

- [ ] RED-test every coordinator failure boundary, producer start before sink,
  stop with active producers, metrics actor removal, flush timeout/error, and
  observability destruction last.
- [ ] Register observability start before producer components.
- [ ] Register shutdown so network/actors/scheduler quiesce, metrics actor is
  removed, metrics/traces/logs flush, ports close, and managers stop.
- [ ] Ensure optional exporter degradation follows explicit blueprint policy;
  required sink failure fails startup/readiness.
- [ ] Expose bounded queue/drop/exporter/flush state in snapshot and lifecycle
  errors.
- [ ] Run fault matrix and commit as
  `refactor: order runtime telemetry lifecycle`.

---

### Task 5: Define typed cluster runtime and factory

- [ ] RED-test disabled optional state, missing factory, factory failure,
  start/stop, node events, snapshots, legacy typed views, and virtual
  destruction without concrete core includes.
- [ ] Add `IClusterRuntime`, `ClusterRuntimeConfig/Dependencies`,
  `ClusterSnapshot`, `ClusterStopRequest`, and typed factory function.
- [ ] Keep interface limited to lifecycle, node-event ingress, snapshot, health/
  readiness contribution, and temporary typed legacy views.
- [ ] Implement concrete aggregate owning failure model, singleton coordination,
  and route invalidation in the cluster library.
- [ ] Register/inject factory through `RuntimeBuilder`; fail validation when
  required cluster mode has no factory.
- [ ] Commit as `refactor: add typed cluster runtime boundary`.

---

### Task 6: Replace type erasure and route cluster lifecycle/events

- [ ] RED-test failure-model transitions, singleton election/invalidation,
  discovery node-event/stop race, event after gate close, callback drain,
  coordinator rollback, and public accessor parity.
- [ ] Replace three `unique_ptr<void, cleanup_fn>` fields with one optional
  `unique_ptr<IClusterRuntime>` owned by `Impl`/coordinator graph.
- [ ] Route `NetworkRuntime` membership events through a stable
  `ClusterNodeEventSink`; remove facade captures.
- [ ] Start/stop cluster in coordinator dependency order and include its health
  in readiness policy.
- [ ] Forward legacy concrete accessors through explicit `ClusterLegacyViews`;
  perform no cast in core.
- [ ] Delete cleanup functions, void ownership, and old observer wiring.
- [ ] Run cluster/net/lifecycle tests and commit as
  `refactor: replace type-erased cluster ownership`.

---

### Task 7: Add bounded operations snapshot service

- [ ] RED-test collection order, per-component epochs, collection interval,
  pagination/bounds, concurrent component mutation, unavailable optional
  components, and formatter/I/O lock assertions.
- [ ] Add fixed snapshot ports for lifecycle, actor, messaging, stream, network,
  observability, and cluster owners.
- [ ] Implement sequential copy aggregation; release each component lock before
  acquiring another or allocating/formatting aggregate output.
- [ ] Carry collection start/end and per-component epochs; document
  best-effort/non-atomic semantics.
- [ ] Add bounded page tokens for actor/stream listings and reject unbounded
  admin requests.
- [ ] Commit as `refactor: add component operations snapshots`.

---

### Task 8: Migrate CLI, admin, and health inspection

- [ ] RED snapshot fixtures for existing output plus lifecycle, degraded,
  queue/drop, network, stream, and cluster fields.
- [ ] Replace direct `ActorSystem` registry/manager/transport/cluster traversal
  with `OperationsSnapshotService` values.
- [ ] Render/serialize only after snapshot collection returns.
- [ ] Add tests that fail if formatting, logger, actor callback, or socket write
  occurs while a component lock is held.
- [ ] Preserve command/API names and document bounded pagination/truncation.
- [ ] Commit as `refactor: serve operations from runtime snapshots`.

---

### Task 9: Remove facade owners and enforce boundaries

- [ ] Delete facade/shell metrics/log/trace owner fields and internal raw
  setters after all producers use ports.
- [ ] Retain public raw compatibility forwards with explicit comments; forbid
  their use under `src/` except the adapter implementation.
- [ ] Add architecture checks rejecting telemetry managers/rings as facade
  fields, `unique_ptr<void>` cluster ownership, casts from cluster void state,
  new `ActorSystem&` component dependencies, and direct CLI/admin owner access.
- [ ] Update ownership inventory and commit as
  `refactor: enforce observability and cluster boundaries`.

---

### Task 10: Verification and Phase 8 handoff

- [ ] Run focused metrics/log/trace/reload/actor/delivery/net/cluster/CLI/admin/
  health/lifecycle and architecture tests.
- [ ] Run coordinator failure matrix with observability and cluster stages.
- [ ] Run ASan repeated start-fail, reload-fail, cluster-fail, flush-timeout,
  stop/destroy loops.
- [ ] Run TSAN telemetry producer/reload/flush, node-event/cluster-stop, and
  snapshot/concurrent-mutation tests.
- [ ] Compile examples/apps using public telemetry/cluster accessors and run
  the full suite because ownership and operations surfaces are cross-cutting.
- [ ] Verify forbidden patterns:

```bash
rg -n "unique_ptr<void|cluster_cleanup_fn|static_cast<.*Cluster" \
  include/hpactor/actor src/actor src/runtime
rg -n "set_metrics_ring_buffer|set_logger" src --glob '!**/*compat*'
rg -n "ActorSystem[&*]" src/runtime src/cluster
git diff --check
git status --short
```

- [ ] Update `CLAUDE_MEMORY.md` with final owners, lifecycle/flush order,
  cluster interface, snapshot semantics, evidence, and Phase 8 adapter list.
- [ ] Commit as `test: verify observability and cluster boundaries`.

## Definition of Done

- [ ] One component owns metrics/log/trace infrastructure.
- [ ] Stable ports outlive all producers and close only after quiescence.
- [ ] Metrics actor remains exclusively actor-owned.
- [ ] Core contains no type-erased cluster ownership/downcast.
- [ ] Typed optional cluster lifecycle and node events are tested.
- [ ] CLI/admin/health use bounded copied snapshots only.
- [ ] Existing public accessors compile through adapters.
- [ ] Focused, full-suite, architecture, ASan, and TSAN evidence passes.
