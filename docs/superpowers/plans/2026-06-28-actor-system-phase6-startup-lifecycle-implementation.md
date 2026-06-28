# ActorSystem Phase 6 Startup Blueprint and Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` or `superpowers:executing-plans`.
> Invoke `.claude/skills/tddflow-development/` before production edits and
> `superpowers:verification-before-completion` before commits or completion.

**Goal:** Validate immutable startup input before side effects and make one
coordinator own start, reverse rollback, readiness, reload serialization,
drain, stop, and destructor teardown.

**Architecture:** `RuntimeBlueprintBuilder` parses/normalizes; immutable
`RuntimeBlueprint` carries effective subsystem values; `RuntimeBuilder`
constructs but does not start the graph; `RuntimeCoordinator` mediates
lifecycle order without absorbing subsystem policy. Factory and legacy
constructor use the same path.

**Tech Stack:** C++20, CMake/Ninja, GoogleTest, HPActor `result<T>`, opaque
`TomlTableView`, fixed lifecycle descriptors, subprocess process-mode tests,
ASan/TSAN, no RTTI/exceptions.

## Global Constraints

- Begin only after Phase 5 is merged and its one-loop/callback-lifetime
  evidence passes.
- Work in `.claude/worktrees/actor-system-runtime-lifecycle/` on branch
  `refactor/actor-system-runtime-lifecycle` from updated `origin/main`.
- Verify path/branch before every write and use worktree-local build dirs.
- RED -> GREEN -> REFACTOR for every production task.
- Preferred creation performs no side effect before complete blueprint
  validation.
- Legacy constructor/factory must share one blueprint, builder, and graph.
- Preserve existing constructor and `load_topology()` source signatures.
- No coordinator lock across component calls, joins, actor work, or callbacks.
- Unknown/unregistered reload fields default to `RestartRequired`.
- Never replace stable dependencies whose raw addresses escaped.
- Add no service locator, generic DI container, config reflection framework,
  public `toml++`, exception control flow, or runtime downcast.

## Expected File Structure

**Create:**

- `src/runtime/runtime_blueprint.hpp/.cpp`
- `src/runtime/runtime_blueprint_builder.hpp/.cpp`
- `src/runtime/runtime_builder.hpp/.cpp`
- `src/runtime/runtime_coordinator.hpp/.cpp`
- `src/runtime/runtime_lifecycle_ports.hpp`
- `include/hpactor/runtime/runtime_lifecycle_snapshot.hpp`
- `include/hpactor/config/reload_report.hpp`
- unit tests for blueprint, builder, coordinator, and reload classification
- integration tests for startup failure, topology reload, readiness/shutdown
- `tests/architecture/assert_runtime_lifecycle_boundaries.cmake`

**Modify:** subsystem parser registrations; `ActorSystem::Impl`; facade
construction/topology/shutdown; process manager preflight; component lifecycle
surfaces; CMake; architecture inventory; and `CLAUDE_MEMORY.md`.

---

### Task 0: Establish worktree, baseline, and side-effect ledger

- [ ] Create
  `.claude/worktrees/actor-system-runtime-lifecycle` on
  `refactor/actor-system-runtime-lifecycle` from `origin/main`.
- [ ] Verify Phase 5 `NetworkRuntime` and start/stop contracts exist.
- [ ] Read repository rules, concurrency rules, production plane, all Phase
  1–6 designs, and current lifetime inventory.
- [ ] Configure/build focused runtime/config/actor/net/process tests.
- [ ] Record every constructor, topology, shutdown, destructor, process, thread,
  listener, actor-spawn, and readiness side effect in actual order.
- [ ] Add RED characterization for current config precedence, legacy call-site
  compilation, topology failure mutation, readiness, and shutdown order.
- [ ] Commit baseline as `test: characterize ActorSystem startup lifecycle`.

---

### Task 1: Build immutable blueprint values and parser

**Deliverable:** Complete effective input can be built and validated without
creating runtime resources.

- [ ] RED-test config-only, config+TOML, topology-only defaults, precedence,
  provenance, deterministic fingerprint, all actor-factory validation,
  invalid cross-field combinations, and zero observed side effects.
- [ ] Define private immutable component configs and `ConfiguredActorSpec`.
- [ ] Implement `RuntimeBlueprint` with const access only and bounded
  provenance/fingerprint data.
- [ ] Implement builder entry points using subsystem-registered parsers and
  opaque `TomlTableView`; keep `toml++` out of public headers.
- [ ] Validate endpoint/ports, bounds, feature dependencies, actor ids/names/
  behaviors, and process constraints before returning a blueprint.
- [ ] Add a side-effect probe test that fails on thread creation, listen,
  daemonization, actor spawn, timer registration, or singleton initialization.
- [ ] Run config/blueprint tests and commit as
  `refactor: add immutable runtime blueprint`.

---

### Task 2: Register subsystem-owned reload descriptors

**Deliverable:** Every effective field has an owner and explicit reload class.

- [ ] RED-test representative `Live`, `RestartRequired`, and `Immutable`
  fields, unknown-field default, mixed-diff rejection, and stable diff order.
- [ ] Add fixed `ConfigPathId`, `ReloadClass`, and descriptor registration.
- [ ] Register descriptors beside actor, messaging, stream, network, scheduler,
  process, logging, tracing, and metrics parsers—not in one facade switch.
- [ ] Add duplicate-path/missing-effective-field architecture tests.
- [ ] Implement complete blueprint diff without storing references to temporary
  TOML nodes.
- [ ] Classify a field `Live` only after its owner supplies prepare/commit/
  rollback semantics; otherwise use `RestartRequired`.
- [ ] Commit as `refactor: classify runtime configuration reloads`.

---

### Task 3: Add the side-effect-free runtime builder

**Deliverable:** One builder creates the complete, stopped component graph.

- [ ] RED-test enabled/disabled component graphs, dependency addresses,
  construction-failure cleanup, no started threads/listeners/timers, and graph
  parity from equivalent config/topology inputs.
- [ ] Add small subsystem construction functions and typed dependency bundle.
- [ ] Implement `RuntimeBuilder::build(blueprint)` returning stopped `Impl` plus
  coordinator registration. Do not call component `start()`.
- [ ] Ensure optional network is absent, not replaced by dummy services.
- [ ] Ensure all port targets are constructed before consumers and member/
  destruction order is explicit.
- [ ] Add deterministic construction fault injection and leak checks.
- [ ] Commit as `refactor: add side-effect-free runtime builder`.

---

### Task 4: Implement coordinator state machine and rollback ledger

**Deliverable:** Fake-component tests exhaustively prove start/rollback/stop.

- [ ] RED-test all legal/illegal states, concurrent callers, reentrant shutdown,
  deferred network-thread stop, primary vs rollback errors, and terminal result
  stability.
- [ ] Define fixed lifecycle descriptors/tokens and transition epochs.
- [ ] Implement owner-thread serialized operations without holding coordinator
  state lock across component calls.
- [ ] On each successful start stage push one rollback token; on failure invoke
  all tokens in exact reverse and record bounded rollback failure bits.
- [ ] Make stop idempotent from built, starting, running, failed, draining,
  stopping, and stopped states.
- [ ] Make destructor call the same stop implementation, with no parallel
  teardown branch.
- [ ] Run exhaustive fake tests and commit as
  `refactor: add RuntimeCoordinator lifecycle state machine`.

---

### Task 5: Move real startup behind coordinator stages

**Deliverable:** The actual runtime starts only through the reviewed dependency
order and readiness barrier.

- [ ] RED-test failure before/after process preflight, telemetry, scheduler/
  actor, messaging, stream/router, topology adoption, network prepare/activate,
  `SystemInit`, and readiness publication.
- [ ] Split process preflight from thread-starting stages; test daemon/fork
  behavior in subprocesses.
- [ ] If needed, split Phase 5 network prepare/listen from discovery/external
  activation so the node cannot publish early.
- [ ] Register actual component stages with explicit names and rollback actions.
- [ ] Deliver configured actors and `SystemInit` in deterministic blueprint
  order through canonical actor adoption.
- [ ] Publish readiness only after every enabled stage and activation barrier.
- [ ] Remove constructor-owned start calls as each stage migrates.
- [ ] Run startup/network/topology/process tests and commit as
  `refactor: coordinate ActorSystem startup`.

---

### Task 6: Converge shutdown, destructor, signals, and failed startup

**Deliverable:** One stop path and dependency-safe stop order.

- [ ] RED-test graceful drain, deadline/abort, active RPC/network callbacks,
  queued actor work, signal/admin request, concurrent destructor/public stop,
  and repeated stop.
- [ ] Make readiness false before closing ingress or beginning drain.
- [ ] Implement reviewed sequence: close publication/ingress; drain actors;
  cancel remaining remote work; stop/join network; stop stream maintenance;
  stop/join scheduler; flush current telemetry; remove process/fault hooks.
- [ ] Route `ActorSystem::shutdown`, shutdown phases, signal/admin requests,
  constructor failure cleanup, and destructor through coordinator.
- [ ] Delete the old destructor and shutdown teardown branches.
- [ ] Verify no component is destroyed until its stop/quiescence token completes.
- [ ] Commit as `refactor: unify ActorSystem shutdown lifecycle`.

---

### Task 7: Add preferred factory and preserve constructor compatibility

**Deliverable:** Result-returning creation reports failures; the legacy
constructor remains safe and uses the same graph.

- [ ] RED-test `create(config)`, `create(config,path)`, validation/start errors,
  no side effects on validation error, and factory/constructor graph parity.
- [ ] Add static result-returning factory APIs without changing existing
  constructor call sites.
- [ ] Implement both paths through `RuntimeBlueprintBuilder` and
  `RuntimeBuilder`; prohibit a compatibility-only component graph.
- [ ] On legacy failure retain a valid non-ready stopped object, bounded
  `startup_status()`, and safe operation results/no-ops.
- [ ] Compile existing examples/apps unchanged and add compile-only legacy
  coverage.
- [ ] Document factory as preferred without premature constructor removal.
- [ ] Commit as `feat: add result-returning ActorSystem creation`.

---

### Task 8: Convert topology loading into validated reload/deployment

**Deliverable:** Late topology cannot silently mutate startup-only state or
partially apply a rejected config diff.

- [ ] RED-test invalid actor behavior before mutation, immutable/restart diff
  rejection, mixed diff atomicity, live prepare failure, commit failure/
  rollback, stable DLQ identity, topology spawn failure report, and invalid
  lifecycle states.
- [ ] Build a complete candidate blueprint and diff before invoking any owner.
- [ ] Add subsystem `prepare_reload`/`commit_reload`/`rollback_reload` only for
  fields proven `Live`.
- [ ] Update active blueprint/fingerprint only after full live commit.
- [ ] Implement configured-actor deployment as a separately reported
  transaction with explicit spawned/initialized/failed/rolled-back counts.
- [ ] Make pre-start `load_topology()` replace the pending validated blueprint;
  running calls use reload/deployment; other states reject.
- [ ] Delete direct facade config/DLQ/tracing/process/transport mutation.
- [ ] Commit as `refactor: make topology reload lifecycle-safe`.

---

### Task 9: Add lifecycle snapshots and architecture enforcement

**Deliverable:** Operations can explain startup/readiness/failure without
reaching into coordinator internals.

- [ ] Add bounded `RuntimeLifecycleSnapshot` and `ReloadReport` tests.
- [ ] Expose state, readiness, epoch, stage, primary/rollback errors, shutdown
  deadline status, and blueprint fingerprint.
- [ ] Update health/readiness and CLI/admin adapters to consume snapshots.
- [ ] Add architecture checks rejecting component start/stop in facade
  constructor/destructor, direct readiness writes, TOML parsing by components,
  and a second shutdown sequence.
- [ ] Update lifetime inventory/runbook and commit as
  `docs: record coordinated runtime lifecycle`.

---

### Task 10: Verification matrix and handoff

- [ ] Run focused runtime/config/process/actor/messaging/stream/net/topology/
  shutdown and architecture targets.
- [ ] Run deterministic failure at every construction/start/activation stage;
  compare exact reverse rollback order and live-resource counters.
- [ ] Run reload-class matrix and verify rejected candidates cause zero visible
  mutation.
- [ ] Run ASan repeated validate-fail/destroy, start-fail/destroy,
  reload-fail/stop, and start/stop/destroy loops.
- [ ] Run TSAN concurrent snapshot/readiness/shutdown/reload requests and
  network-thread stop deferral.
- [ ] Compile all examples/apps and run the full test suite because startup and
  public construction are cross-cutting.
- [ ] Check forbidden patterns and diff:

```bash
rg -n "TomlParser::parse|scheduler_->start|network_runtime_->start" \
  src/actor/actor_system.cpp
rg -n "is_ready_\.store" src --glob '!runtime_coordinator.cpp'
rg -n "~ActorSystem|ActorSystem::shutdown" src/actor/actor_system.cpp
git diff --check
git status --short
```

- [ ] Update `CLAUDE_MEMORY.md` with factory policy, legacy failure behavior,
  reload classes, lifecycle order, fault matrix, and Phase 7 handoff.
- [ ] Commit evidence as `test: verify coordinated ActorSystem lifecycle`.

## Definition of Done

- [ ] Preferred creation validates complete input before any side effect.
- [ ] Blueprint is immutable and has deterministic provenance/fingerprint.
- [ ] Builder constructs one stopped graph used by both APIs.
- [ ] All start stages and reverse rollbacks have deterministic tests.
- [ ] One coordinator owns readiness and every stop path.
- [ ] Late topology rejects disallowed changes before mutation.
- [ ] Stable runtime dependency identities survive live reload.
- [ ] Process preflight occurs before any runtime thread.
- [ ] Existing call sites compile unchanged.
- [ ] Focused, full-suite, architecture, ASan, and TSAN evidence passes.
