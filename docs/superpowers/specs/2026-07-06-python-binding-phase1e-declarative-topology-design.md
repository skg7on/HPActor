<!--
Copyright 2026 HPActor Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Python Binding Phase 1E Declarative Topology Design

**Status:** Approved design

**Date:** 2026-07-06

**Depends on:** Python binding Phases 1A through 1D and the runtime blueprint,
coordinator, actor-spawner, TOML parser-registry, and topology foundations

## 1. Summary

Phase 1E lets an application declare Python actors in the existing HPActor
`[[actors]]` topology list by using a namespaced behavior reference:

```toml
[[actors]]
id = "echo"
behavior = "python:my_app.actors:Echo"
args = { prefix = "production" }
```

The existing TOML parser continues to own imports, template inheritance,
topological ordering, mailbox settings, supervision, and system configuration.
Phase 1E does not add a parallel `[[python.actors]]` schema and does not parse
TOML in Python.

Before any configured actor is externally published, the Python runtime imports
every referenced module, resolves and validates every class, validates
constructor keyword arguments, and freezes an immutable factory manifest. A
mixed C++/Python topology then starts as one transaction under the runtime
coordinator. Actor names and `SystemInit` are committed only after every Python
constructor, behavior, and `on_start()` succeeds. Any failure rolls back all
actors created by the transaction in reverse order.

No HPActor scheduler worker imports a module, calls CPython, acquires the GIL,
or waits for Python. Python preflight and object lifecycle run only on the
dedicated Python asyncio thread. Native startup components exchange bounded,
value-only records and integer factory tokens with that thread.

## 2. Context and Problem

Phases 1A through 1D establish imperative Python actor spawn, restart,
shutdown, observability, and packaging. The umbrella design deliberately
deferred declarative topology until those lifecycle contracts were explicit.

HPActor already has a declarative topology pipeline:

```text
TOML -> TomlParser -> TopologyModel -> configured actor bootstrap -> SystemInit
```

The topology model stores a behavior string and string-valued actor arguments.
The C++ factory registry resolves exact behavior names. Current and planned
runtime foundations also provide an immutable runtime blueprint, a coordinator
with reverse-order startup rollback, and an actor spawner with publication
rollback.

Python actor classes add four requirements that an exact C++ factory lookup
does not cover:

1. A behavior string must identify an importable Python module and class.
2. Importing and class inspection require the GIL and must run on the Python
   runtime thread.
3. A Python bridge is not ready until construction, behavior validation, and
   `on_start()` finish.
4. Partial topology publication must not survive import, construction, start,
   name-registration, or later actor failures.

Phase 1E introduces one configured-actor provider seam and one transactional
bootstrap path rather than special-casing Python throughout the TOML parser.

## 3. Goals

1. Declare C++ and Python actors together in the existing `[[actors]]` list.
2. Resolve `python:<module>:<qualname>` references without a Python TOML parser.
3. Require an application-supplied allowlist before importing topology modules.
4. Validate every Python reference and constructor binding before actor
   publication.
5. Preserve the Phase 1 execution-domain, queue-boundedness, generation, and
   shutdown contracts.
6. Start a mixed topology transactionally and roll it back in reverse order.
7. Keep existing C++ behavior names and imperative Python spawn source
   compatible.
8. Expose structured Python errors and bounded native observability for every
   preflight, start, commit, and rollback phase.
9. Keep topology actors local to the process and protobuf-only for messages.

## 4. Non-Goals

Phase 1E does not:

- add a separate `[[python.actors]]` document;
- parse TOML, resolve TOML imports, or apply templates in Python;
- load Python code from file paths, URLs, archives, entry points, or modified
  `sys.path` entries;
- treat an allowlist declared by the same topology file as a security boundary;
- add topology hot reload, actor-tree diffing, or live Python class replacement;
- add Python actors to the binary topology format;
- add remote Python placement or make Python an independent cluster node;
- infer or register protobuf `TypeTag` values from topology;
- pass arbitrary typed TOML values, pickle data, JSON objects, or Python object
  graphs as constructor arguments;
- preserve Python module or class state across rollback;
- unload imported Python modules from `sys.modules` after a failed startup;
- change the semantics of ordinary C++ `ActorSystem::load_topology()` calls.

Python topology is startup-only. Dynamic children continue to use
`ActorSystem.spawn()` or `ActorContext.spawn()`.

## 5. Topology Contract

### 5.1 Behavior reference syntax

A Python behavior reference has this exact grammar:

```text
python:<module>:<qualname>
```

`<module>` and every dotted module segment must match
`[A-Za-z_][A-Za-z0-9_]*`. `<qualname>` uses the same dotted-segment grammar so
module-level classes and nested classes are representable. Empty segments,
relative imports, slashes, backslashes, whitespace, `<locals>`, NUL bytes, and
more than one module/class separator are rejected.

The module and qualified class name are each limited to 255 UTF-8 bytes. The
complete behavior value is limited to 518 bytes. These checks happen in native
preparation before the Python runtime thread starts importing modules.

Examples:

```toml
behavior = "python:my_app.actors:Echo"
behavior = "python:my_app.workers:Workers.Ingest"
```

Plain behavior values such as `worker` continue through the exact C++ factory
registry. In a Python-enabled topology, the `python:` prefix is reserved for
the Python provider. Preflight rejects a collision with an exact C++ factory of
the same name rather than silently choosing one.

### 5.2 Actor arguments

Phase 1E preserves the existing `ActorDef.args` contract:

```text
unordered_map<string, string>
```

The Python runtime passes a frozen copy as keyword arguments:

```python
actor_class(**frozen_args)
```

TOML strings remain strings. Existing parser canonicalization also turns
integer, floating-point, and Boolean scalar values into strings before they
reach the provider. Arrays, inline nested tables, byte strings, positional
arguments, and object deserialization are rejected.

Keys must be valid Python identifiers, must not start with `__hpactor_`, and
must be at most 128 UTF-8 bytes. Each value is limited to 4096 UTF-8 bytes, each
actor has at most 128 arguments, and the combined key/value payload per actor
is limited to 64 KiB. The configured actor count remains bounded by
`max_actor_bindings`.

The Python preflight uses `inspect.signature(actor_class).bind(**args)` without
constructing the actor. A required positional-only parameter, missing required
keyword, or unexpected keyword fails preflight. Constructor execution remains
part of transactional startup because signature binding cannot prove that user
code will succeed.

### 5.3 Example

```toml
[system.python]
enabled = true
topology_start_timeout_ms = 30000

[[actors]]
id = "echo"
behavior = "python:my_app.actors:Echo"
args = { prefix = "prod" }
mailbox_capacity = 4096

[[actors]]
id = "audit"
behavior = "audit_sink"
supervisor = "echo"
```

The existing parser owns all fields except the interpretation of a
`python:`-prefixed behavior value. No edit to `parse_file_data()` or public
exposure of `toml++` is permitted.

## 6. Public Python API

### 6.1 Topology policy

```python
@dataclass(frozen=True, slots=True)
class PythonTopologyPolicy:
    allowed_module_prefixes: tuple[str, ...]

    def allows(self, module: str) -> bool: ...
```

An allowed prefix matches only the exact module or a child separated by a dot.
For example, `my_app.actors` permits `my_app.actors` and
`my_app.actors.billing`, but not `my_app.actors_evil`.

`allowed_module_prefixes` must be non-empty when the topology contains a Python
behavior. Prefixes use the same absolute module grammar as behavior references.
The application supplies the policy in code; the topology cannot widen it.

### 6.2 System construction

```python
system = ActorSystem.from_topology(
    "config/topology.toml",
    messages=messages,
    policy=PythonTopologyPolicy(
        allowed_module_prefixes=("my_app.actors",),
    ),
)

async with system:
    echo = system.resolve("echo")
    response = await system.ask(
        echo,
        EchoRequest(value="hello"),
        response_type=EchoResponse,
        timeout=5.0,
    )
```

`from_topology()` is a synchronous, side-effect-free constructor. It stores the
path, message registry, and policy. Parsing, importing, and runtime startup
happen in `__aenter__()` so failures are awaitable and cleanup is deterministic.

The existing imperative constructor and `spawn()` API are unchanged. Calling
`spawn()` after a topology system reaches `Running` remains supported. Calling
`from_topology()` with a binary topology containing Python actors is rejected
with `TopologyError` in Phase 1E.

### 6.3 Errors

```python
class TopologyPhase(Enum):
    PARSE = "parse"
    POLICY = "policy"
    IMPORT = "import"
    CLASS_RESOLUTION = "class_resolution"
    CLASS_VALIDATION = "class_validation"
    CONSTRUCTOR_BINDING = "constructor_binding"
    NATIVE_PREPARE = "native_prepare"
    ACTOR_START = "actor_start"
    COMMIT = "commit"
    ROLLBACK = "rollback"


class TopologyError(HPActorError):
    phase: TopologyPhase
    actor_id: str | None
    behavior: str | None
```

Error detail is bounded to 4096 UTF-8 bytes. Import tracebacks are logged under
the existing bounded Python failure policy but are not embedded unboundedly in
the public exception. Actor argument values are never emitted to errors, logs,
metrics, CLI, or health snapshots because they may contain secrets.

## 7. Native Architecture

### 7.1 Prepared topology

The native topology preparation path parses TOML exactly once and returns an
immutable `PreparedTopology`. It owns the `TopologyModel`, its deterministic
fingerprint, and one `PreparedActorSpec` per actor:

```cpp
enum class ConfiguredActorKind : uint8_t {
    Cpp,
    Python,
};

struct PythonBehaviorRef final {
    std::string module;
    std::string qualname;
};

struct PreparedActorSpec final {
    size_t topology_index{0};
    ConfiguredActorKind kind{ConfiguredActorKind::Cpp};
    std::optional<PythonBehaviorRef> python;
    uint64_t factory_token{0};
};
```

Preparation validates TOML, imports/templates, actor ordering, C++ factory
availability, Python behavior grammar, argument bounds, duplicate actor IDs,
provider collisions, and configured capacity. It performs no imports, thread
creation, listener startup, actor spawn, name registration, or runtime config
mutation.

The CPython limited-API wrapper converts Python specs to immutable tuples of
strings and string dictionaries. It does not retain a `PyObject*` in
`PreparedTopology`, `PythonNativeSystem`, an actor provider, a queue, or a
runtime coordinator stage.

### 7.2 Python factory manifest

The dedicated asyncio thread owns `_TopologyFactoryManifest`. It groups specs
by `(module, qualname)`, imports each module once with
`importlib.import_module()`, resolves the qualified attribute chain with
`getattr()`, and validates that the resolved value:

- is a class;
- is a concrete subclass of `hpactor.Actor`;
- has valid `@actor(...)` metadata;
- does not reuse an actor type name for a different class in the same manifest;
- accepts the configured string keyword arguments;
- exposes no unsupported positional-only requirement.

The manifest freezes after successful validation. Each configured actor gets a
nonzero 64-bit factory token. The token identifies an immutable record
containing the class and frozen arguments on the Python thread. Native code
receives only the token, actor ID, behavior reference, topology index, and
argument fingerprint.

Tokens are scoped to one `PythonNativeSystem` generation. They are never reused
within that generation and are invalid after rollback or shutdown.

### 7.3 Configured actor provider seam

The configured topology bootstrap gains a provider port owned by the runtime
composition root. It is not a TOML parser plugin and it does not call Python:

```cpp
struct ConfiguredActorProviderPort final {
    void* context{nullptr};
    bool (*matches)(void*, const PreparedActorSpec&) noexcept {nullptr};
    result<Actor> (*spawn_unpublished)(
        void*, const config::ActorDef&, const PreparedActorSpec&) noexcept {
        nullptr
    };
    result<void> (*await_ready)(
        void*, ActorId, const PreparedActorSpec&,
        std::chrono::milliseconds) noexcept {nullptr};
    void (*rollback)(
        void*, ActorId, const PreparedActorSpec&) noexcept {nullptr};
};
```

The default C++ provider wraps `ActorFactoryRegistry` and `ActorSpawner`. The
Python provider constructs a `PythonBridgeActor` carrying the factory token and
generation, admits a bounded topology-install record to the Python dispatch
queue, and waits only on the runtime startup thread for a value-only completion.

`await_ready()` is never called from a scheduler worker, event-loop thread,
network thread, or the Python runtime thread. The runtime coordinator stage has
an exact timeout from `topology_start_timeout_ms`. Timeout returns a typed
startup failure and begins rollback; it is not proof of progress in tests.

The port uses function pointers and a context pointer. It introduces no public
`std::function`, RTTI, exception-based dispatch, `PyObject*`, or dependency on
`Python.h`.

### 7.4 Internal versus external publication

The bridge needs an actor ID, mailbox, context, and directory entry before
Python `on_start()` can use ordinary actor operations. Therefore Phase 1E
distinguishes two publication states:

1. **Internal unpublished:** the actor exists in the private directory by ID,
   but has no topology name, receives no external ingress, and is not included
   in committed topology snapshots.
2. **Committed:** the topology name is atomically registered, the actor is
   included in snapshots, and `SystemInit` may be delivered after the complete
   transaction commits.

Internal publication is not readiness. Health readiness remains false and
network/application ingress remains closed until all actors commit.

`ActorSpawner` must return an ownership lease for an internally published
actor. Destroying or explicitly rolling back the lease removes the directory
entry, unregisters scheduler placement, runs actor cleanup, and releases the
mailbox. A committed lease transfers ownership to `ActorRuntime`.

### 7.5 Startup transaction

`TopologyBootstrapTransaction` consumes one immutable prepared model and the
configured providers. Its algorithm is:

1. Validate the complete model and every provider match without mutation.
2. Start the Python runtime/gateway stage, but keep external ingress closed.
3. Import and freeze the Python factory manifest on the Python loop.
4. Bind the value-only factory-token manifest to the native prepared topology.
5. Spawn each actor in existing topological order as internally unpublished.
6. Await configured readiness for that actor. For Python this means constructor,
   behavior freeze, runner installation, and `on_start()` success.
7. Record each successful actor lease in the transaction journal.
8. Atomically register every topology name. A name collision fails the commit.
9. Deliver `SystemInit` to every committed actor.
10. Mark the topology stage committed, open later ingress stages, and allow the
    coordinator to enter `Running` and ready state.

No actor name is visible before step 8. No `SystemInit` is delivered before all
names commit. Actor startup follows the existing topology order, so mixed C++
and Python dependencies retain one deterministic order.

### 7.6 Rollback

Any failure in steps 1 through 9 preserves the first failure as the primary
error and rolls the journal back in reverse order:

1. Keep readiness false and external ingress closed.
2. Remove any names installed during a partial commit.
3. For a Python actor whose `on_start()` completed, run `on_stop()` once on the
   Python loop and remove its runner.
4. Cancel an in-progress Python constructor or `on_start()` and wait for its
   bounded cancellation completion.
5. Stop and unpublish each bridge or C++ actor through its actor-spawner lease.
6. Resolve outstanding topology tokens as startup-aborted.
7. Release Python factory records on the Python loop.
8. Roll back gateway/runtime stages and join the Python runtime thread.
9. Continue runtime-coordinator rollback for previously completed native
   stages.

Rollback is idempotent. Secondary rollback failures are recorded as bounded
error bits and structured logs but never replace the primary failure.

Imported modules remain in `sys.modules`; Python cannot safely unload arbitrary
module code. No actor object, runner, behavior, future, notifier callback,
factory record, bridge, name, directory entry, or scheduler registration from
the failed transaction may remain.

## 8. Lifecycle Stage Order

The Python topology path uses this startup ordering:

```text
validated runtime blueprint
  -> native queues/notifiers
  -> scheduler and actor runtime
  -> Python gateway and dedicated asyncio loop
  -> Python import/class preflight
  -> mixed topology bootstrap transaction
  -> network/application ingress
  -> readiness = true
```

Shutdown uses the reverse ownership order already established by Phase 1C.
Configured Python actors are ordinary Python runners after commit; they do not
have a separate shutdown implementation.

An import or policy failure occurs before actor spawn and listener startup. A
constructor, behavior, or `on_start()` failure occurs inside the topology stage
and triggers both topology-journal rollback and normal coordinator rollback.

## 9. Configuration

Phase 1E adds one field to the existing subsystem-owned `[system.python]`
parser:

```toml
[system.python]
topology_start_timeout_ms = 30000
```

The default is 30,000 ms. Valid values are 100 through 300,000 ms inclusive.
Invalid values fail native preparation before thread creation. The timeout is a
deadlock guard for one actor constructor/behavior/`on_start()` sequence, not a
sleep-based test mechanism and not a total-topology deadline.

The module allowlist is deliberately application code, not TOML configuration.
The topology file cannot grant itself permission to import additional code.

Topology changes are `RestartRequired`. Phase 1E does not add a live reload
applier for module references, classes, actor args, or actor membership.

## 10. Execution-Domain and Ownership Rules

- TOML parsing and native lexical validation run on the startup caller before
  component start.
- Python import, `inspect`, class objects, factory records, actor instances,
  behaviors, `on_start()`, and `on_stop()` live on the dedicated Python thread.
- Scheduler workers execute only `PythonBridgeActor` and value-only HPActor
  operations.
- The startup coordinator may wait for a value-only readiness completion; no
  scheduler, network, event-loop, or Python thread may perform that wait.
- Cross-thread records contain strings, IDs, fingerprints, generations,
  factory tokens, status codes, and bounded details only.
- All install/completion admissions use the existing bounded bridge queues.
- Accepted enqueue transfers record ownership to the consumer. Rejected
  enqueue leaves ownership with the startup transaction and fails startup.
- Every record carries native-system generation and actor generation. Stale
  preflight, install, ready, commit, or rollback completions are discarded and
  counted.
- Native code never retains borrowed Python memory or a Python class/object
  pointer.

## 11. Failure Semantics

| Failure | Phase | Observable result | Mutation allowed |
| --- | --- | --- | --- |
| Invalid TOML/import/template/DAG | Parse | `TopologyError(PARSE)` | None |
| Python reference grammar/limits | Native prepare | `TopologyError(NATIVE_PREPARE)` | None |
| Module outside application allowlist | Policy | `TopologyError(POLICY)` | None |
| Module import failure | Import | `TopologyError(IMPORT)` | `sys.modules` cache only |
| Missing/non-class attribute | Class resolution | `TopologyError(CLASS_RESOLUTION)` | Factory manifest discarded |
| Not an `Actor` or invalid decorator | Class validation | `TopologyError(CLASS_VALIDATION)` | Factory manifest discarded |
| Constructor signature mismatch | Constructor binding | `TopologyError(CONSTRUCTOR_BINDING)` | Factory manifest discarded |
| Constructor/behavior/`on_start()` error | Actor start | `TopologyError(ACTOR_START)` | Full topology rollback |
| Bounded queue rejection | Actor start | `ResourceExhausted` cause | Full topology rollback |
| Per-actor startup timeout | Actor start | `Timeout` cause | Full topology rollback |
| Duplicate/failed name registration | Commit | `TopologyError(COMMIT)` | Full topology rollback |
| Cleanup failure | Rollback | Primary error plus rollback bits | Continue reverse cleanup |

An `ActorError` raised from `on_start()` is still a startup failure; there is no
request sender to receive an application error reply. Unhandled exceptions are
bounded using the Phase 1C failure formatting contract.

## 12. Observability and Operations

Phase 1E adds bounded metrics:

- `hpactor_python_topology_preflight_total{outcome}`
- `hpactor_python_topology_import_total{outcome}`
- `hpactor_python_topology_actor_start_total{outcome}`
- `hpactor_python_topology_rollback_total{outcome}`
- `hpactor_python_topology_start_duration_seconds`
- `hpactor_python_topology_actor_count`

Structured logs include topology fingerprint, phase, actor ID, bounded behavior
reference, module, qualified class name, generation, primary error code, and
rollback bits. They never include actor argument values.

The existing `/python status` snapshot gains a topology subsection with
fingerprint, configured Python actor count, prepared/started/committed counts,
current phase, failure phase, rollback count, and last bounded actor ID. It does
not expose class objects, Python actor memory, constructor arguments, or
tracebacks.

Health liveness remains true while a bounded startup rollback is progressing.
Readiness remains false through preflight, bootstrap, commit, and rollback. A
failed topology leaves the system failed and not ready; it never falls through
to an imperative empty system.

## 13. Security Model

A Python topology is executable configuration. Phase 1E treats the topology
file as trusted deployment input but still requires an application-side module
allowlist to prevent a changed file from importing arbitrary installed modules.

The loader:

- uses `importlib.import_module()` with absolute names only;
- never accepts a filesystem path or URL;
- never mutates `sys.path`;
- never calls package entry-point discovery;
- never evaluates an expression;
- resolves only validated attribute segments with `getattr()`;
- never logs argument values;
- rejects module names outside the application-provided allowlist;
- relies on normal Python packaging and OS permissions for code provenance.

This is not a sandbox. Importing an allowed module executes its top-level code.
Applications must keep the topology and allowed package installation under the
same deployment trust controls as native executable configuration.

## 14. Compatibility

- Existing TOML actors with plain C++ behavior names are unchanged.
- Existing C++ factory registration APIs remain source compatible.
- Existing imperative Python `ActorSystem`, `spawn()`, and context APIs remain
  source compatible.
- `python:` is reserved only for a Python-enabled topology provider. Provider
  collision is a validation error, never implicit precedence.
- The public protobuf/`TypeTag` contract is unchanged.
- The current binary topology format is unchanged. Python behaviors in binary
  topology are rejected until a separate binary-format compatibility design
  defines versioning, fingerprints, and deployment validation.
- A runtime blueprint fingerprint includes topology behavior strings, actor
  argument key/value bytes, and Python topology policy fingerprint. Therefore a
  class reference, argument, or allowlist change is restart-required.

The implementation plan must target the post-Phase-1D branch after it is
updated with the current runtime blueprint and actor-spawner architecture.

## 15. Testing Strategy

### 15.1 Native unit tests

- behavior-reference grammar and byte limits;
- argument count, key, value, and combined-size bounds;
- prepared topology classification and fingerprints;
- C++ registry/provider collision handling;
- actor lease commit and rollback;
- topology journal reverse ordering and idempotency;
- primary error preservation and rollback error bits;
- provider queue rejection and stale-generation completion.

### 15.2 Python unit tests

- exact and child-prefix allowlist matching;
- rejection of lookalike, relative, path, whitespace, and `<locals>` names;
- module imported once for repeated actor classes;
- nested qualified class resolution;
- non-class, non-Actor, abstract, undecorated, and duplicate-type rejection;
- constructor keyword binding with canonical string values;
- factory token uniqueness, freeze, generation, and runtime-loop ownership;
- bounded `TopologyError` fields without argument disclosure.

### 15.3 Integration tests

- one Python actor loaded from TOML and resolved by committed name;
- mixed C++ and Python actors in deterministic topology order;
- templates and TOML imports producing Python actor definitions;
- Python actor send/ask/reply after `SystemInit`;
- constructor, behavior, and `on_start()` failure rollback;
- later C++ actor failure rolling back earlier Python actors;
- name collision during commit rolling back every actor;
- bounded queue rejection and startup timeout rollback;
- no actor names, directory entries, runners, futures, or bridge bindings after
  failure;
- readiness false until commit and false after failed rollback;
- normal imperative spawn after a successful topology start;
- clean shutdown of configured actors in reverse spawn order.

Tests use explicit gates and injected completions. Sleeps and scheduler timing
are never proof of startup or rollback progress.

### 15.4 Architecture and packaging tests

- no `Python.h`, `PyObject`, CPython call, RTTI, exception syntax, or public
  `std::function` in native provider/transaction code;
- no TOML parsing in Python and no edit growing `parse_file_data()`;
- no `toml++` header in a public binding or provider interface;
- no Python pointer in prepared models, queues, journals, blueprints, or
  lifecycle stages;
- installed-wheel topology example works from outside the checkout;
- source and wheel tests reject binary Python topology in Phase 1E;
- Linux/macOS and CPython 3.11 through 3.14 wheel smoke includes one topology
  actor lifecycle.

## 16. Acceptance Criteria

Phase 1E is complete only when all of the following are proven:

1. `python:<module>:<qualname>` actors share the existing topology model,
   ordering, mailbox, supervision, and `SystemInit` flow with C++ actors.
2. The application-side allowlist is required and cannot be widened by TOML.
3. Every module/class/signature is validated before external actor publication
   or ingress.
4. Python constructors, behaviors, lifecycle hooks, registries, and objects are
   touched only on the dedicated asyncio thread.
5. Scheduler and network threads never acquire the GIL or wait for Python.
6. Every configured actor remains externally unpublished until the full mixed
   topology reaches commit.
7. Import, construction, behavior, start, capacity, timeout, later-actor, and
   commit failures roll back the full transaction in reverse order.
8. Failed startup leaves no actor name, bridge, directory entry, scheduler
   registration, runner, future, notifier callback, or factory record.
9. Primary error and secondary rollback evidence are both observable without
   leaking actor arguments.
10. Existing C++ topology and imperative Python tests remain source compatible.
11. Python topology smoke passes from an installed ABI3 wheel on all four Phase
    1D targets and supported CPython minors.
12. The manual states that Python topology is trusted, allowlisted, startup-only,
    local-process-only, TOML-only, and not a hot-reload or remote-placement API.

## 17. Alternatives Rejected

### Separate `[[python.actors]]` tables

This makes language explicit but duplicates actor identity, mailbox,
supervision, dependencies, templates, and ordering. Mixed C++/Python dependency
validation becomes a second merge problem. A namespaced behavior keeps one
topology graph.

### Explicit class registration only

Requiring `system.register_actor(Echo)` avoids dynamic imports but does not meet
the approved module/class discovery goal and forces imperative setup before a
declarative topology can validate itself.

### Parse TOML again with `tomllib`

A Python parser would drift from HPActor imports, templates, canonical scalar
conversion, validation, ordering, and future schema changes. The native parser
must remain the single topology authority.

### Store Python classes in native factories

Capturing `PyObject*` in `ActorFactoryRegistry`, closures, queues, or runtime
stages violates the language-boundary ownership contract and makes destruction
thread-affinity unsafe. Integer factory tokens preserve the boundary.

### Publish each actor as soon as `on_start()` succeeds

Incremental external publication exposes a partially initialized topology and
makes later failure rollback observable to traffic. Internal unpublished leases
allow actor-owned startup work without committing the topology early.

### Unload modules after rollback

Python provides no safe general module-unload contract. Removing entries from
`sys.modules` would leave referenced objects and extension state alive while
pretending rollback was complete. Phase 1E releases binding-owned records but
does not claim to undo import side effects.

## 18. Delivery Boundary

Phase 1E is the final in-process Python binding phase. Its implementation plan
must be independently reviewable from Phase 2 external clients and must begin
only after Phases 1A through 1D pass their acceptance gates.

Phase 2 remains a separate design and plan for pure-Python health, metrics,
gateway, and CLI clients. Native remote-node participation, authenticated
administration, binary Python topology, and topology hot reload each require
their own later designs.
