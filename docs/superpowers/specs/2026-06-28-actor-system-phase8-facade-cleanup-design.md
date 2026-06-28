# ActorSystem Phase 8 Facade and Compatibility Cleanup Design

**Date:** 2026-06-28

**Status:** Proposed final phase design

**Parent design:**
`docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`

**Prerequisites:** Phases 0–7 are merged with their normal, architecture,
integration, ASan, and TSAN evidence. All runtime resources have one component
owner; lifecycle uses a blueprint/builder/coordinator; operations surfaces use
snapshots; typed ports replace internal facade callbacks.

**Scope:** Remove temporary internal migration adapters and dead setters,
deprecate unsafe public raw accessors where replacements exist, reduce the
public `ActorSystem` header through PImpl/forward declarations, expose stable
narrow facade views, enforce dependency/ownership rules, and finish architecture
and runbook documentation. Preserve source compatibility for the current
release; destructive public API removal requires a separately approved major
version plan.

## 1. Summary

Phase 8 makes the achieved architecture visible in the public and build
boundaries. It does not introduce a new runtime subsystem:

```text
public ActorSystem
  +-- one std::unique_ptr<Impl>
  +-- creation/lifecycle compatibility methods
  +-- narrow non-owning views: actors(), messaging(), network(), operations()
  +-- deprecated legacy raw accessors (forward only)

private ActorSystem::Impl
  +-- immutable blueprint
  +-- RuntimeBuilder-created component owners
  +-- RuntimeCoordinator
  +-- no subsystem policy, duplicate state, or service lookup
```

PImpl is an encapsulation mechanism, not permission to hide another God Class.
`Impl` may own the component graph and wire typed ports; it may not reabsorb
spawn, delivery, protocol, network, telemetry, cluster, config, or lifecycle
policy. New features extend their owning component and snapshot/port contract,
not `ActorSystem` fields or switches.

Temporary compatibility is divided explicitly:

- internal migration adapters/setters are removed in this phase;
- unsafe public raw pointer accessors remain source-compatible but are marked
  `[[deprecated]]` when a safe view/snapshot exists;
- removals and ABI promises are outside this phase and require a major-version
  compatibility decision.

## 2. Current-to-Target Surface

After Phase 7 the likely remaining debt includes:

- facade declarations for raw scheduler, transport, loop, discovery, registry,
  metrics/log/trace, cluster, stream, and manager pointers;
- setters retained only to bridge staged extraction;
- inline/template spawn code that pulls many internal types into the public
  header;
- broad transitive includes for net, metrics, mailbox, scheduler, stream,
  topology, CLI, tracing, logging, and cluster implementation details;
- `Impl` helpers that were temporary forwarding points;
- duplicated compatibility state used only to preserve old access paths; and
- architecture docs that still describe ownership as `ActorSystem` fields.

Phase 8 begins with an exact inventory generated from code/call graph and
compile consumers. No adapter is removed merely because its name looks old.

## 3. Important Correctness Findings

### 3.1 PImpl can conceal rather than solve God Class behavior

Moving fields/methods wholesale into `Impl` would improve header size but leave
central policy and change coupling intact.

Required contract: `Impl` owns named components and wiring only. Architecture
checks reject subsystem resource types, policy branches, frame/delivery
classification, actor adoption, TOML parsing, and independent start/stop logic
inside facade/Impl.

### 3.2 Public raw pointers can outlive runtime state

Legacy accessors expose transport, event loop, managers, actors, cluster
objects, and registries without lifetime tokens. A caller can retain a pointer
after stop or reload.

Required contract:

- add safe narrow views/snapshots whose methods validate lifecycle and return
  results/value copies;
- mark raw accessors deprecated with a concrete replacement;
- make raw adapters return null when the component is absent/not available as
  already documented; and
- do not promise safety for retained legacy pointers, but document their valid
  lifetime precisely.

### 3.3 Removing adapters too early can break supported source compatibility

Examples, applications, downstream users, and cluster-linked builds may depend
on old signatures even if core code no longer does.

Required contract: internal adapters may be deleted after a zero-call-site
proof. Public APIs remain for the current release unless already private or
explicitly approved for removal. Compile-only compatibility fixtures and all
examples/apps are part of acceptance evidence.

### 3.4 Header reduction can break template spawn APIs

`spawn<T>` requires enough type information for constraints, construction, and
return types. Blindly forward-declaring all actor/mailbox types can cause
incomplete-type failures in downstream translation units.

Required contract: keep only the minimal public template contract inline;
route non-type-specific adoption through a private/non-template bridge. Add
standalone include tests for every supported spawn form with no reliance on
incidental umbrella includes.

### 3.5 Deprecation attributes can become build-breaking under `-Werror`

Applying `[[deprecated]]` while repository code still uses an accessor—or
without scoping warning policy—can fail builds.

Required contract: migrate all in-repo production/tests/examples first, add
deprecation second, and give each diagnostic a specific replacement. Dedicated
compatibility fixtures suppress only the expected diagnostic locally.

### 3.6 Removing setters can change initialization ordering

Late setters may encode an undocumented but required ordering between stable
ports and producers. Deleting them before constructor injection parity can
produce null/no-op telemetry or delivery targets.

Required contract: for each setter, prove one construction-time injection and
lifetime owner, add a test that the dependency is installed before producer
start, then delete setter and duplicate nullable state.

### 3.7 Snapshot/view methods can accidentally become service locators

A broad `runtime().get<T>()` or `component(name)` API would recreate global
lookup and make ownership implicit.

Required contract: expose a small fixed set of capability-oriented views with
reviewed operations. No templated lookup, string lookup, `void*`, `std::any`,
or public access to private component classes.

### 3.8 Include cleanup can silently change feature/link boundaries

Users may currently receive cluster, HTTP, CLI, or metrics declarations through
`actor_system.hpp`. Removing transitive includes is healthy but can reveal
undocumented dependencies in examples and installed-package consumers.

Required contract: source compatibility applies to declared `ActorSystem`
APIs, not arbitrary incidental includes. Repository consumers add direct
includes for the symbols they use. Release notes list removed transitive
includes, and installed-package compile tests cover supported minimal headers.

## 4. Goals and Non-Goals

### 4.1 Goals

- A small public facade with one private implementation pointer.
- No subsystem policy or resource fields in facade/Impl.
- Safe, explicit, capability-oriented views and copied operations snapshots.
- Removal of all proven-dead internal adapters/setters/duplicate state.
- Actionable public deprecations without current-release source removal.
- Smaller include graph and downstream compile footprint.
- Automated rules that keep future features out of the facade.
- Current architecture, migration, operations, and runbook documentation.

### 4.2 Non-goals

- A major-version API deletion or ABI guarantee.
- Redesigning actor semantics, transport, telemetry, cluster, or lifecycle.
- Adding mixins, facade inheritance, generic service lookup, or public
  component ownership.
- Optimizing compile time at the expense of unclear supported include
  contracts.
- Removing raw APIs that have no safe replacement in this release.

## 5. Target Architecture

### 5.1 Public facade

`ActorSystem` publicly declares construction/factory/lifecycle, existing actor
operations, and narrow views. Its only owning data member is
`std::unique_ptr<Impl> impl_`.

```cpp
class ActorSystem final {
public:
    class Impl;

    static result<std::unique_ptr<ActorSystem>> create(const Config&) noexcept;
    ~ActorSystem();

    ActorSystem(ActorSystem&&) noexcept;
    ActorSystem& operator=(ActorSystem&&) noexcept;
    ActorSystem(const ActorSystem&) = delete;

    ActorSystemActorsView actors() noexcept;
    ActorSystemMessagingView messaging() noexcept;
    ActorSystemNetworkView network() noexcept;
    ActorSystemOperationsView operations() const noexcept;

private:
    std::unique_ptr<Impl> impl_;
};
```

Move operations are included only if existing semantic constraints allow them;
otherwise they remain deleted and the design records that decision.

### 5.2 Narrow views

Views are small non-owning handles containing an opaque context plus fixed
functions, or a private `Impl*` unavailable to applications. They expose
capabilities, not component objects:

- actors: spawn/stop/lookup via stable handles and bounded actor snapshots;
- messaging: delivery result APIs and bounded messaging snapshot;
- network: optional state, safe send/ask/remote spawn, network snapshot;
- operations: aggregate/lifecycle/health snapshots.

Views check lifecycle epoch/state per operation. They are not safe to retain
after the parent system is destroyed; this is documented. Long-lived
cross-thread operations return existing owning handles/results rather than raw
component pointers.

### 5.3 Template spawn bridge

Public templates construct the actor-specific object/spec and call a
non-template private bridge implemented out of line. Canonical adoption,
mailbox selection, directory registration, rollback, metrics, and lifecycle
checks remain in `ActorRuntime`.

Compile tests cover untyped, typed, coroutine, detached/system-supported, and
configured spawn forms currently public. The bridge does not add RTTI or
exception flow.

### 5.4 Compatibility ledger

Every legacy API is classified:

| Class | Phase 8 action |
|---|---|
| private/internal adapter, zero call sites | remove |
| late setter replaced by constructor port | remove after ordering test |
| public raw pointer with safe replacement | retain + deprecate |
| public API without replacement | retain, document, backlog replacement |
| accidental transitive include | remove; consumers include directly |
| obsolete duplicate state | remove after parity/invariant test |

The ledger records symbol, visibility, repository/external risk, replacement,
deprecation release, earliest removal release, and verification fixture.

### 5.5 `Impl` boundary rules

Allowed:

- immutable blueprint and component-owner members produced by builder;
- coordinator and fixed wiring ports;
- out-of-line compatibility forwarding;
- no-policy view construction.

Forbidden:

- subsystem algorithm/state duplicated from an owner;
- switch statements classifying messages/frames/config fields;
- direct TOML parsing;
- actor directory manipulation or mailbox policy;
- direct component start/stop sequence;
- generic lookup by type/name; and
- callbacks captured by long-lived subsystems.

### 5.6 Public include strategy

`actor_system.hpp` includes only types required to declare supported methods and
instantiate required templates. Heavy value definitions move to focused public
headers; implementation types are forward-declared. Each public header is
self-contained and compiled alone in C/C++ package tests where appropriate.

An include-budget check records direct/transitive header count and preprocessed
size for a minimal ActorSystem consumer. The budget prevents regression but is
not a substitute for correctness.

## 6. Compatibility and Release Policy

- Source compatibility for documented public APIs is mandatory in this phase.
- No ABI stability is newly promised; PImpl improves future stability but may
  change current layout and requires normal release notes.
- Deprecations name the replacement and earliest major release for removal.
- Runtime behavior/defaults, wire/protobuf contracts, TypeTags, actor addresses,
  and lifecycle semantics remain unchanged from Phase 7.
- Removed incidental includes are listed with required direct headers.
- Compatibility fixtures represent supported external usage and remain after
  internal adapters are deleted.

## 7. Architecture Enforcement

Automated checks fail on:

- subsystem resource fields in `ActorSystem` or `Impl` outside the approved
  component-owner list;
- `ActorSystem&/*` stored in runtime components;
- new internal calls to deprecated raw accessors;
- generic lookup (`get<T>`, string service name, `void*`, `std::any`);
- lifecycle calls outside coordinator/builder tests;
- TOML parser use outside config composition;
- protocol/delivery policy in facade/Impl;
- non-self-contained public headers; and
- compatibility setters with no approved ledger entry.

Checks should prefer compile/dependency tests and focused structural scripts;
fragile regex is used only for narrow forbidden patterns.

## 8. Migration Sequence

1. Generate compatibility/include/ownership ledger and freeze baseline.
2. Add narrow public views and migrate repository consumers.
3. Move template-independent facade implementation out of line.
4. Introduce one PImpl pointer and preserve builder/component ownership.
5. Remove dead internal adapters, setters, and duplicate state one by one.
6. Add public deprecations after all repository production call sites migrate.
7. Reduce includes and fix consumers to include their actual dependencies.
8. Add architecture/include-budget/package compatibility gates.
9. Update architecture, operations, migration, release, and runbook docs.

## 9. Testing and Verification

- API compile matrix for legacy and new views.
- Standalone public header and installed-package consumer compilation.
- All template spawn variants from minimal includes.
- Enabled/disabled/stopped/failed lifecycle behavior for views/adapters.
- Null/lifetime behavior for deprecated raw pointers.
- Architecture owner/forbidden-dependency checks.
- Include graph/preprocessed-size baseline and regression budget.
- All examples/apps, full normal suite, ASan, TSAN, and representative
  build-option matrix.
- Wire/protobuf/TypeTag golden checks to prove cleanup is behavior-neutral.

## 10. Acceptance Criteria

1. `ActorSystem` has one owning `impl_` member and no subsystem resource field.
2. Constructor/destructor/facade/Impl contain no subsystem policy or independent
   lifecycle sequence.
3. Each runtime resource appears under exactly one documented component owner.
4. All internal migration adapters/setters without approved use are removed.
5. Unsafe public raw accessors with replacements are actionable deprecations.
6. New code uses narrow views/ports/snapshots, not raw facade internals.
7. Public headers are self-contained and substantially less coupled.
8. Existing documented call sites/examples/apps compile without source change,
   aside from adding direct includes for previously incidental symbols.
9. Architecture checks prevent God Class regression.
10. Architecture/runbooks/release notes describe actual implemented behavior.

## 11. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| PImpl becomes hidden God Object | Explicit allowed/forbidden rules and structural checks |
| Deprecation breaks `-Werror` | Migrate repository first; local suppression only in compatibility fixture |
| Header cleanup breaks templates | Minimal-include compile matrix for every spawn form |
| Raw accessor removal breaks users | Retain/deprecate this release; major-version removal separately approved |
| Views become service locator | Fixed capability methods; no generic lookup or component exposure |
| Adapter deletion changes ordering | Per-setter injection/producer-start test before removal |

## 12. Decision Summary

- Use PImpl to expose the component architecture, not hide central policy.
- Keep one public facade and add fixed capability-oriented views.
- Remove internal migration debt now; deprecate public unsafe access gradually.
- Preserve template API with an out-of-line canonical adoption bridge.
- Treat transitive include cleanup as an explicit compatibility/release item.
- Enforce ownership/dependency rules so `ActorSystem` cannot regrow into a God
  Class.
