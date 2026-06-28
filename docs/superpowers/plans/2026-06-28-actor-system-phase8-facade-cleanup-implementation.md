# ActorSystem Phase 8 Facade and Compatibility Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` or `superpowers:executing-plans`.
> Invoke `.claude/skills/tddflow-development/` before production edits and
> `superpowers:verification-before-completion` before commits/completion.

**Goal:** Finish the refactor with a small PImpl facade, capability-oriented
views, no internal migration debt, controlled public deprecations, smaller
include boundaries, and automated regression guards.

**Architecture:** `ActorSystem` owns one `Impl`; `Impl` holds builder-produced
component owners/coordinator and typed wiring only. New views expose safe
operations/snapshots without component ownership. Legacy public raw accessors
forward and are deprecated where replacements exist.

**Tech Stack:** C++20, CMake/Ninja, compile-only/package tests, include graph and
preprocessor metrics, GoogleTest/CTest, ASan/TSAN, no RTTI/exceptions.

## Global Constraints

- Start only after Phase 7 is merged and all owner/lifecycle/snapshot evidence
  passes.
- Work in `.claude/worktrees/actor-system-facade-cleanup/` on branch
  `refactor/actor-system-facade-cleanup` from updated `origin/main`.
- Verify path/branch before writes; use RED -> GREEN -> REFACTOR.
- Preserve documented public source compatibility for this release.
- Do not remove a public symbol merely because no repository call site exists.
- Internal adapter/setter removal requires zero-call-site proof plus replacement
  ordering/parity test.
- Add deprecation only after repository production/tests/examples migrate.
- PImpl/Impl may wire/own components but may not absorb subsystem policy.
- No generic component/service lookup, public private-component type, mixin
  decomposition, `void*` owner, `std::any`, RTTI, or exception flow.
- Full build/test is required because public headers and construction are
  cross-cutting.

## Expected File Structure

**Create:**

- focused public view headers under `include/hpactor/actor/` or
  `include/hpactor/runtime/`
- `src/runtime/actor_system_impl.hpp/.cpp` if not already canonical
- compatibility compile fixtures under `tests/compatibility/`
- standalone public-header/package tests
- `tests/architecture/assert_actor_system_facade_boundaries.cmake`
- `docs/architecture/actor/actor-system-component-architecture.md`
- migration/release/runbook updates

**Modify:** `actor_system.hpp/.cpp`; runtime Impl/builder; repository consumers
of raw accessors/incidental includes; CMake export/install/test definitions;
architecture docs; project outline; memory.

---

### Task 0: Establish worktree and generate compatibility ledger

- [ ] Create/verify the prescribed worktree and branch.
- [ ] Verify Phase 7 owner, interface, snapshot, and architecture checks.
- [ ] Read repository rules and every phase design/evidence summary.
- [ ] Configure/build full baseline with default options.
- [ ] Use graph/search and compiler dependency output to inventory every public
  method/type, internal adapter/setter, facade/Impl field, direct/transitive
  include, repository call site, example/app consumer, and feature-gated
  consumer.
- [ ] Create a ledger with visibility, owner, replacement, external risk,
  current action, deprecation release, earliest removal, and test fixture.
- [ ] Record baseline header direct/transitive count, preprocessed bytes,
  compile time (informational), object size, and public layout policy.
- [ ] Commit as `docs: inventory ActorSystem compatibility surface`.

---

### Task 1: Add capability-oriented facade views

- [ ] RED-test actor, messaging, network, and operations views in running,
  disabled, stopping, stopped, and failed-start states.
- [ ] Define the smallest fixed view APIs based on existing safe component
  operations/results/snapshots; expose no private runtime class.
- [ ] Implement lifecycle validation and optional component errors.
- [ ] Document parent lifetime: views cannot outlive `ActorSystem`; operations
  returning async/owning state use existing handles/results.
- [ ] Add compile tests proving no generic type/name lookup is available.
- [ ] Migrate repository production code, CLI/admin, examples where appropriate,
  and new tests to views.
- [ ] Commit as `feat: add narrow ActorSystem capability views`.

---

### Task 2: Isolate public template spawn from adoption internals

- [ ] RED compile minimal translation units for every supported spawn form,
  including typed/untyped/coroutine/configured/system-supported variants.
- [ ] Identify which actor construction/spec types must remain visible.
- [ ] Move non-type-specific lifecycle validation, mailbox/adoption, directory,
  rollback, and metrics work to an out-of-line bridge that delegates to
  `ActorRuntime`.
- [ ] Keep only type construction/constraints and bridge call inline.
- [ ] Prove address, generation, mailbox policy, context, initialization,
  failure rollback, and return types match Phase 2 behavior.
- [ ] Commit as `refactor: isolate ActorSystem spawn templates`.

---

### Task 3: Complete one-pointer PImpl without creating a hidden God Class

- [ ] RED architecture-test allowed/forbidden fields and policy patterns.
- [ ] Move public data layout to one `std::unique_ptr<Impl>` and make destructor
  out of line where incomplete-type rules require it.
- [ ] Preserve or explicitly decide move construction based on existing
  semantics; do not add it solely for PImpl convenience.
- [ ] Ensure `Impl` members are named component owners, immutable blueprint,
  coordinator, and fixed ports only.
- [ ] Move facade methods out of line as thin forwards.
- [ ] Reject TOML parsing, frame/message switches, actor-directory mutation,
  component start/stop sequences, and generic lookup in facade/Impl.
- [ ] Run construction/lifecycle/API parity tests and commit as
  `refactor: complete ActorSystem PImpl facade`.

---

### Task 4: Remove temporary internal adapters and late setters

Execute one ledger entry at a time.

- [ ] For each adapter/setter, graph-search all call sites including tests,
  examples, apps, feature branches, and cluster/proactor builds.
- [ ] Add RED/parity test proving the replacement dependency is installed
  before its producer starts and remains valid through producer quiescence.
- [ ] Migrate the last call site to constructor port/component API.
- [ ] Delete the adapter/setter and duplicate nullable/cache state.
- [ ] Run the narrow owning subsystem tests before proceeding to the next item.
- [ ] Add structural checks for removed migration APIs so they cannot return.
- [ ] Commit in reviewable owner-based groups, e.g.
  `refactor: remove telemetry migration setters` and
  `refactor: remove network compatibility wiring`.

---

### Task 5: Deprecate unsafe public raw accessors

- [ ] Confirm every proposed deprecation has a safe concrete replacement and
  zero repository production use.
- [ ] Add compile tests for legacy accessor signature/constness/null behavior
  and new replacement behavior.
- [ ] Apply `[[deprecated("use ...")]]` with an exact view/snapshot method and
  planned major-version removal note.
- [ ] Suppress the expected warning only inside dedicated compatibility fixtures;
  do not globally weaken warnings.
- [ ] Retain undecidable/no-replacement raw APIs, document risk, and create a
  backlog entry instead of premature removal.
- [ ] Verify `-Werror` builds for normal repository consumers.
- [ ] Commit as `api: deprecate unsafe ActorSystem raw accessors`.

---

### Task 6: Reduce public includes and make headers self-contained

- [ ] RED-test each installed public header in a standalone translation unit and
  minimal ActorSystem/spawn/view consumers.
- [ ] Generate include graph and classify includes as declaration-required,
  template-required, or incidental.
- [ ] Move heavy definitions to focused headers; forward-declare only where
  complete-type/destructor/template rules permit.
- [ ] Update repository consumers to include symbols they actually use.
- [ ] Keep documented ActorSystem method declarations source-compatible; list
  removed incidental includes in migration/release notes.
- [ ] Add include-budget regression checks for direct/transitive count and
  preprocessed size using stable tolerances rather than machine timing alone.
- [ ] Verify install/export package consumers with feature options on/off.
- [ ] Commit as `refactor: reduce ActorSystem public header coupling`.

---

### Task 7: Add permanent architecture and compatibility gates

- [ ] Add compile/dependency checks rejecting subsystem resource fields in
  facade/Impl outside approved component-owner types.
- [ ] Reject stored `ActorSystem&/*` in runtime components, generic lookup,
  internal deprecated-accessor calls, direct lifecycle calls outside
  coordinator, direct TOML parsing outside config, and policy switches in
  facade/Impl.
- [ ] Add supported legacy API compile fixtures and new view fixtures.
- [ ] Add wire/protobuf/TypeTag golden checks to this phase's verification
  target even though no protocol change is intended.
- [ ] Prefer AST/compile/dependency checks; keep regex checks narrow and
  documented to avoid false confidence.
- [ ] Commit as `test: enforce ActorSystem facade boundaries`.

---

### Task 8: Update architecture, operations, migration, and runbooks

- [ ] Document the final component/port/owner graph, blueprint/builder/
  coordinator flow, start/stop order, telemetry flush, cluster boundary, and
  snapshot semantics.
- [ ] Update project outline and memory to distinguish implemented runtime
  behavior from backlog items.
- [ ] Add migration table from deprecated raw accessors to views/snapshots.
- [ ] Document removed incidental includes and direct replacements.
- [ ] Update shutdown/startup failure/readiness/telemetry/cluster incident
  runbooks with actual snapshot/error fields.
- [ ] Record source/ABI policy and earliest possible major-version removals.
- [ ] Validate doc links and commit as
  `docs: finalize ActorSystem component architecture`.

---

### Task 9: Final cross-project verification

**Deliverable:** Evidence that the eight-phase refactor is complete and
behavior-compatible.

- [ ] Run focused facade/runtime/actor/messaging/stream/net/observability/
  cluster/config/process/CLI/admin/health/architecture tests.
- [ ] Run all standalone-header, installed-package, compatibility, minimal
  spawn, and include-budget tests.
- [ ] Build examples/apps and representative option matrix:
  default, examples/apps disabled, proactor enabled, CLI disabled, metrics
  disabled, and relevant cluster-linked targets.
- [ ] Run the complete normal test suite.
- [ ] Run ASan on lifecycle, actor, messaging, stream, net, telemetry, cluster,
  and compatibility integration tests.
- [ ] Run TSAN on mailbox/scheduler, stream registry, network stop, telemetry
  reload/flush, cluster events, and operations snapshots.
- [ ] Compare focused performance baselines for local/remote delivery, disabled/
  enabled telemetry ports, network framing, and ActorSystem consumer compile
  footprint. Investigate material regressions; do not hide them by widening
  budgets without review.
- [ ] Verify ownership and forbidden patterns:

```bash
rg -n "unique_ptr<.*(Transport|EventLoop|LogManager|TraceManager)|unique_ptr<void" \
  include/hpactor/actor/actor_system.hpp src/runtime/actor_system_impl.*
rg -n "ActorSystem[&*]" src/runtime src/net src/cluster
rg -n "get<.*>|service.*lookup|std::any|void\*.*component" \
  include/hpactor/actor src/runtime
git diff --check
git status --short
```

- [ ] Review ledger: every entry is retained, deprecated, or removed with
  evidence; no “temporary” item is unclassified.
- [ ] Request code/architecture review and resolve findings with
  `superpowers:receiving-code-review`.
- [ ] Commit final evidence as `test: verify ActorSystem component refactor`.

## Definition of Done

- [ ] Facade has one owning PImpl pointer and no subsystem policy/resource.
- [ ] Impl is a composition root, not a hidden God Class.
- [ ] Narrow views cover safe new internal/external usage.
- [ ] All proven-dead internal adapters/setters/duplicate state are removed.
- [ ] Unsafe public raw accessors with replacements are deprecated, not silently
  removed.
- [ ] Spawn templates compile from minimal supported includes and retain parity.
- [ ] Public headers are self-contained with a guarded include budget.
- [ ] Architecture checks prevent central ownership/policy regression.
- [ ] Architecture, migration, release, and runbook docs match implementation.
- [ ] Full normal, option-matrix, compatibility, ASan, and TSAN evidence passes.
