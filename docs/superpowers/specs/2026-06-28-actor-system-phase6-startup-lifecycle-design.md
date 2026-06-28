# ActorSystem Phase 6 Startup Blueprint and Lifecycle Design

**Date:** 2026-06-28

**Status:** Proposed phase design

**Parent design:**
`docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`

**Prerequisites:** Phases 0–5 are merged. `ActorRuntime`, `MessagingRuntime`,
`StreamRuntime`, `InboundFrameRouter`, and optional `NetworkRuntime` have
explicit owners, stable ports, and independently testable lifecycle surfaces.

**Scope:** Add immutable validated startup input (`RuntimeBlueprint`), one
component assembler (`RuntimeBuilder`), result-returning preferred creation,
reload classification, and a `RuntimeCoordinator` that owns startup rollback,
readiness transitions, drain, stop, and destructor teardown. Preserve the
legacy constructor and `load_topology()` as compatibility adapters with
explicit failure/reload behavior.

## 1. Summary

Phase 6 separates three concerns that are currently interleaved:

```text
Config/TOML/topology input
  -> RuntimeBlueprintBuilder       (parse, normalize, validate, classify)
       -> immutable RuntimeBlueprint
            -> RuntimeBuilder      (construct component graph; no start)
                 -> RuntimeCoordinator
                      start: component order + rollback
                      stop: readiness -> drain -> reverse stop
                 -> ActorSystem facade
```

The preferred API parses optional topology and validates all startup-only
configuration before daemonization, scheduler threads, logging/tracing
threads, discovery publication, network listen, system actor creation, or
other runtime side effects.

`RuntimeBuilder` creates one component graph. Both the new result-returning
factory and the legacy constructor use it. The coordinator is a mediator
without subsystem policy: it knows dependency order and lifecycle state, but
does not parse TOML, deliver messages, spawn actor internals, route frames, or
configure transport details.

`load_topology()` stops being a second startup path. Before start it can
replace a pending blueprint. While running it performs a validated reload
transaction containing only fields declared `Live`; `RestartRequired` and
`Immutable` changes are rejected before any mutation or actor spawn.

## 2. Current-State Evidence

The current `ActorSystem` constructor is nearly 300 lines and starts multiple
subsystems while building them: logging, process manager/daemonization,
scheduler, tracing, discovery, transport listen, network thread, system actors,
CLI/receptionist, and fault injection.

`load_topology()` executes later and:

- parses TOML after constructor side effects;
- mutates system and mailbox fields in `config_`;
- replaces the owned dead-letter queue while delivery dependencies may retain
  its old address;
- starts/stops tracing through `apply_tracing_config()`;
- changes process configuration after daemonization;
- updates transport pool configuration;
- validates actor factories only after several mutations; and
- spawns configured actors before the method can report all possible errors.

The destructor has a second independent stop sequence. The existing shutdown
coordinator covers selected drain steps but constructor failure, destructor,
and public shutdown do not share one state machine and rollback ledger.

## 3. Important Correctness Findings

### 3.1 Configuration is applied after irreversible side effects

Topology may change scheduler, process, mailbox, network, or telemetry values
after the relevant component already started. The stored config can therefore
claim a state the runtime never adopted.

Required contract: the preferred create path parses and validates the complete
effective startup blueprint before any runtime side effect. Every effective
field has provenance and a reload class.

### 3.2 `load_topology()` can partially mutate before returning failure

Unknown actor behavior is validated only after metrics/logging/config/DLQ/
tracing/process/transport fields have changed. A later actor factory or spawn
failure can leave partial state and partial actors.

Required contract:

- validate the complete candidate and actor factory set first;
- classify the full diff before applying any part;
- reject a diff containing `Immutable` or `RestartRequired` fields atomically;
- prepare all live changes before commit;
- apply/rollback live changes through subsystem-owned transactions; and
- topology actor changes use an explicit deployment transaction with named
  partial-failure semantics, never accidental interleaving with config reload.

### 3.3 Replacing stable runtime dependencies can create dangling pointers

The current topology path replaces `dead_letters_`; earlier delivery
components may retain its old raw address. Similar risks exist for telemetry
sinks and callback targets.

Required contract: startup-owned dependency objects have stable identity for
the runtime lifetime. Live reload mutates supported policy inside the owner or
atomically swaps a reference-counted immutable policy; it never replaces an
object whose raw address escaped.

### 3.4 Constructor failure cannot be represented without exceptions

The project forbids exception-based control flow, but a C++ constructor cannot
return `result<T>`. Starting threads and listeners inside it makes failure
reporting and rollback ambiguous.

Required contract:

- add `ActorSystem::create(...) -> result<std::unique_ptr<ActorSystem>>` as the
  preferred API;
- the legacy constructor delegates to the same builder and records a bounded
  `startup_status()`;
- if legacy startup fails, it produces a valid non-ready stopped object whose
  operations return the recorded startup error/no-op according to their
  current contract; and
- do not terminate, throw, or leave a partially running object.

### 3.5 Startup order is implicit and distributed

Scheduler work can begin before later callback targets, telemetry, or system
actors are ready. Network publication may expose a node before topology
bootstrap finishes.

Required contract: one reviewed dependency order gates each stage. Readiness
becomes true only after all enabled components, system actors, topology actors,
and publication barriers succeed.

### 3.6 Destructor, shutdown, and failed startup use different teardown paths

Independent cleanup paths drift, omit stages, or reverse the wrong dependency.

Required contract: `RuntimeCoordinator::stop()` is the only teardown
implementation. Public shutdown, signal/admin shutdown, failed-start rollback,
legacy failure cleanup, and destructor all converge on it.

### 3.7 Readiness can remain true while ingress or drain changes

Readiness is currently a mutable atomic reached from callbacks, not a derived
lifecycle guarantee.

Required contract: readiness is owned by the coordinator. It becomes true only
at `Running`, becomes false before any ingress/drain transition, and never
returns true from `Stopping`, `Stopped`, or `Failed`.

### 3.8 Process initialization has special pre-thread constraints

Daemonization may fork and therefore must occur before any worker or telemetry
thread. Treating all start stages uniformly could violate this invariant.

Required contract: blueprint validation precedes everything; process-mode
preflight/daemonization is an explicit coordinator phase before any component
thread. Its irreversible actions are reported distinctly and tested in a
subprocess, not hidden in a component constructor.

### 3.9 Rollback itself can fail or be re-entered

Stop operations can return errors, callbacks can request shutdown during
startup, and a network-thread stop may require owner-thread completion.

Required contract: the coordinator records the primary startup error, attempts
every eligible reverse action, accumulates bounded rollback error bits, handles
deferred joins from its owner thread, and reaches a stable terminal state.

## 4. Goals and Non-Goals

### 4.1 Goals

- Complete pre-side-effect validation in the preferred API.
- One immutable effective blueprint and component graph.
- One explicit startup/rollback/stop state machine.
- Atomic reload classification and subsystem-owned live changes.
- Accurate readiness and inspectable failure stage.
- Source-compatible legacy construction/topology APIs.
- Deterministic fault injection at every lifecycle boundary.

### 4.2 Non-goals

- Telemetry owner extraction and cluster typing (Phase 7).
- PImpl/include cleanup and compatibility removal (Phase 8).
- Hot reload for every field; unsupported fields are explicitly classified.
- Generic reflection, runtime service locator, DI container, or plugin system.
- Transactional external effects beyond documented compensation semantics.
- Wire/protobuf, mailbox, scheduling, delivery, or discovery behavior changes.

## 5. Target Architecture

### 5.1 Immutable `RuntimeBlueprint`

The blueprint contains normalized component configs, endpoint/node identity,
validated topology actor specs, enabled-component flags, process preflight,
resource bounds, and a bounded provenance/fingerprint record.

```cpp
class RuntimeBlueprint final {
public:
    const ActorRuntimeConfig& actor() const noexcept;
    const MessagingRuntimeConfig& messaging() const noexcept;
    const StreamRuntimeConfig& streams() const noexcept;
    const std::optional<NetworkRuntimeConfig>& network() const noexcept;
    std::span<const ConfiguredActorSpec> actors() const noexcept;
    uint64_t fingerprint() const noexcept;
};
```

There are no public setters. Validation creates the object; component code does
not parse TOML or read the mutable legacy `ActorSystem::Config`.

### 5.2 Blueprint builder

`RuntimeBlueprintBuilder` supports:

- `from_config(config)`;
- `from_config_and_topology(config, path)`;
- `from_topology(path)` with documented defaults; and
- `diff(current, candidate)`.

It runs self-registering subsystem parsers using opaque `TomlTableView`,
resolves precedence once, checks cross-component invariants, validates all
actor factories without spawning, and returns typed errors with a config path
and provenance. Public headers do not expose `toml++`.

### 5.3 Reload classification

Each subsystem owns descriptors for its fields:

```cpp
enum class ReloadClass : uint8_t { Live, RestartRequired, Immutable };

struct ConfigFieldDescriptor {
    ConfigPathId path;
    ReloadClass reload_class;
    result<void> (*validate_change)(const void*, const void*) noexcept;
};
```

Examples:

| Field | Class | Reason |
|---|---|---|
| endpoint/node identity | Immutable | Address and ownership identity |
| scheduler threads/backend | RestartRequired | Worker topology |
| TLS/listen/discovery mode | RestartRequired | Network resources |
| mailbox algorithm/capacity default | RestartRequired | Existing mailbox allocation |
| logging level/category filter | Live | Owner-supported policy swap |
| trace sampling/export policy | Live if manager supports prepare/commit | Otherwise restart-required |
| pool outbound limits | Live only with atomic pool transaction | Validate before apply |
| process daemon/pidfile mode | Immutable after preflight | Process-level effect |

Unknown fields default to `RestartRequired`, never silently `Live`.

### 5.4 Runtime builder

`RuntimeBuilder` receives a blueprint and injectable factories/fault hooks. It
allocates the component graph in dependency order but does not call `start()`.
It returns an `ActorSystem::Impl` plus a coordinator with all optional stages
registered.

It is composition-root code, not a permanent switch for subsystem policy.
Component-specific construction remains in small factory functions owned by
their subsystem.

### 5.5 Runtime coordinator

```cpp
class RuntimeCoordinator final {
public:
    result<void> start() noexcept;
    result<void> stop(ShutdownRequest) noexcept;
    result<ReloadReport> reload(const RuntimeBlueprint&) noexcept;
    RuntimeLifecycleSnapshot snapshot() const noexcept;
};
```

States:

```text
Built -> Preflighting -> Starting -> Running
                    \-> RollingBack -> Failed
Running -> Draining -> Stopping -> Stopped
Built ---------------------------> Stopped
```

Only the coordinator writes lifecycle state/readiness. Calls are serialized;
repeated stop callers observe the same terminal result. Start-after-terminal
is rejected.

### 5.6 Startup order

Exact stages, omitting disabled components:

1. validate blueprint fingerprint and preconditions;
2. process-mode preflight/daemonization before any thread;
3. install process signals/fault hooks that do not launch worker threads;
4. start current telemetry dependencies needed by producers (moved into the
   formal `ObservabilityRuntime` in Phase 7);
5. start scheduler/`ActorRuntime` and core system actor adoption;
6. start `MessagingRuntime` maintenance and stable delivery sinks;
7. start `StreamRuntime`/router ingress in closed state;
8. install configured actors through canonical adoption, still not externally
   ready;
9. start `NetworkRuntime` and pass its progress/listen barrier;
10. deliver `SystemInit` in deterministic topology order;
11. publish discovery membership/external ingress and then readiness;
12. transition to `Running`.

If Phase 5 currently combines network listen and publication, split its start
into prepare/activate operations before implementing this order.

### 5.7 Rollback and stop order

Each successful stage adds a fixed rollback token. Startup failure rolls back
strictly in reverse. Normal stop uses the dependency-safe sequence:

1. readiness false and reject new external work;
2. close network/router ingress and leave discovery;
3. drain actors/mailboxes within deadline;
4. abort remaining RPC/HTTP/reliable work as requested;
5. stop/join `NetworkRuntime`;
6. stop stream/router maintenance;
7. stop actor scheduling and join workers;
8. flush telemetry while sinks and managers still exist;
9. remove process/fault hooks and release component graph;
10. publish `Stopped`.

Phase 7 refines telemetry stage ownership without changing coordinator
semantics.

### 5.8 Preferred and compatibility APIs

Preferred:

```cpp
static result<std::unique_ptr<ActorSystem>> create(const Config&) noexcept;
static result<std::unique_ptr<ActorSystem>>
create(const Config&, const std::string& topology_path) noexcept;
```

Legacy `ActorSystem(const Config&)` delegates to the same blueprint/builder and
starts immediately. On failure it retains a valid stopped `Impl` and exposes
`startup_status()`/lifecycle snapshot. This is a migration compromise, not a
second implementation.

`load_topology(path)` behavior:

- `Built`: validate and replace pending blueprint;
- `Running`: compute diff, reject non-live fields before mutation, then execute
  live reload and a separately reported topology deployment transaction;
- other states: return `InvalidLifecycleState`.

### 5.9 Live reload transaction

Reload has four phases:

1. build and validate the candidate blueprint;
2. compute complete diff and reject disallowed classes;
3. ask affected components to `prepare_reload()` without visible mutation;
4. commit in dependency order, or roll prepared/committed reversible changes
   back in reverse.

If any affected component cannot provide compensation, its field is not
classified `Live`. The current blueprint/fingerprint changes only after full
commit.

Topology actor deployment is reported independently with accepted, already
present, spawned, initialized, failed, and rolled-back counts. Phase 6 does not
claim distributed deployment atomicity.

## 6. Concurrency Contract

- One coordinator owner thread performs start/rollback/final join operations.
- Other lifecycle callers enqueue/wait; they do not execute component teardown.
- No coordinator lock is held across component start/stop/reload, actor
  delivery, join, telemetry, or callback code.
- State transition epochs prevent stale asynchronous completion from advancing
  a newer transition.
- Readiness publication uses release semantics; observers use acquire.
- Component ports remain valid from graph construction through final reverse
  destruction.
- Reload serializes with start/stop and cannot run while draining.

## 7. Error, Readiness, and Snapshot Model

`RuntimeStartError` includes bounded stage, component, error code, blueprint
fingerprint, and rollback-failure bits. `RuntimeLifecycleSnapshot` includes
state, readiness, transition epoch, last successful stage, primary error,
rollback summary, shutdown mode/deadline status, and active blueprint
fingerprint.

Health may be degraded while readiness remains true only for conditions whose
subsystem contract explicitly permits continued service. A lifecycle failure
or active drain always makes readiness false.

## 8. Migration Sequence

1. Characterize current construction/topology/shutdown order and side effects.
2. Add immutable blueprint/value validation with parity tests.
3. Add result-returning factory and side-effect-free graph builder.
4. Add coordinator with fake components and exhaustive fault matrix.
5. Move current startup stages behind coordinator descriptors.
6. Route shutdown/destructor/failure through one stop operation.
7. Add field reload descriptors and atomic diff rejection.
8. Convert `load_topology()` to the compatibility reload/deployment adapter.
9. Add readiness/operations snapshots and remove independent writes.
10. Delete old startup/shutdown branches and duplicate state.

## 9. Testing Strategy

- Blueprint precedence, provenance, cross-field validation, fingerprint
  determinism, unknown-field default classification, and no-side-effect parse.
- Constructor/factory component graph parity and legacy failure safety.
- Deterministic failure before/after every stage with exact reverse rollback.
- Concurrent start/stop/reload/shutdown and network-thread deferred stop.
- Readiness transitions and no early discovery/external publication.
- Reload rejection before mutation; prepare failure; commit/rollback behavior;
  stable DLQ/telemetry identity.
- Topology actor validation before spawn and explicit deployment report.
- Process preflight in subprocess tests before thread count increases.
- Repeated fail/destroy and start/stop/destroy under ASan; lifecycle races under
  TSAN.

## 10. Acceptance Criteria

1. Preferred creation validates complete config/topology before side effects.
2. Blueprint is immutable and component code does not parse TOML/config.
3. Constructor and factory share the same builder/component graph.
4. Every startup stage has deterministic reverse-rollback coverage.
5. Destructor, public shutdown, signals, and failed startup share one stop path.
6. Readiness is coordinator-owned and false before drain/ingress close.
7. `load_topology()` rejects immutable/restart-required changes atomically.
8. Runtime-owned dependency identities cannot dangle during reload.
9. Process-mode preflight precedes all runtime threads.
10. Existing construction and topology call sites compile unchanged.

## 11. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Coordinator becomes another God Class | Lifecycle order only; config parsing and subsystem policy stay with owners |
| Legacy constructor hides startup error | Valid stopped object plus explicit `startup_status()` and operations snapshot |
| Reload claims atomicity it cannot provide | Only compensatable fields are `Live`; topology deployment reported separately |
| Process daemonization is not reversible | Explicit pre-thread phase and distinct irreversible-error semantics |
| Readiness order changes startup behavior | Characterize current consumers and add publication barriers/integration tests |
| Fault matrix becomes timing-dependent | Fixed fake components, barriers, deterministic injected stage failures |

## 12. Decision Summary

- Parse, normalize, and validate once into an immutable blueprint.
- Use typed construction functions, not a runtime DI container.
- Make the coordinator a non-owning lifecycle mediator.
- Prefer result-returning creation; preserve the constructor as one adapter.
- Treat unknown reload fields as restart-required.
- Never mutate startup-only state from late topology loading.
- Use one stop implementation for rollback, shutdown, and destruction.
- Make readiness a lifecycle guarantee rather than an independently writable
  flag.
