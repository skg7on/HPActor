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

# Python Binding Phase 1E Declarative Topology Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add allowlisted `python:<module>:<qualname>` actors to HPActor TOML topology with preflighted Python factories, mixed C++/Python transactional startup, external publication only at commit, and complete reverse-order rollback.

**Architecture:** The existing native TOML parser remains the single topology authority and classifies namespaced Python behavior strings into an immutable prepared topology. A generic configured-actor provider port and `TopologyBootstrapTransaction` compose ordinary C++ factories with a Python provider that exchanges only integer factory tokens and bounded value records with the dedicated asyncio thread. Python imports and objects stay on that thread; the startup worker may wait for readiness, while scheduler/network threads never call or wait for Python.

**Tech Stack:** C++20, HPActor `RuntimeBlueprint`/`RuntimeCoordinator`/`ActorSpawner`/`ActorDirectory`, subsystem-owned TOML parsing, CPython 3.11 Stable ABI, asyncio, `importlib`, `inspect`, GoogleTest, Python `unittest`, CMake/Ninja, ABI3 wheel smoke, Linux and macOS.

## Global Constraints

- Execute only after every acceptance gate in Phases 1A through 1D passes.
- Before implementation, update the implementation branch with current `main` and verify the runtime files use `src/actor/system/actor_system.cpp`, `include/hpactor/runtime/runtime_blueprint.hpp`, and `src/runtime/actor_spawner.cpp`.
- Work in an isolated repository worktree; do not implement directly in the main checkout.
- Python topology syntax is exactly `python:<module>:<qualname>` inside the existing `[[actors]]` list.
- Module and qualified-class segments match `[A-Za-z_][A-Za-z0-9_]*`; module and qualname are each at most 255 UTF-8 bytes and the complete behavior is at most 518 bytes.
- Actor argument keys are Python identifiers, do not start with `__hpactor_`, and are at most 128 UTF-8 bytes. Values are strings of at most 4096 UTF-8 bytes. Each actor has at most 128 arguments and at most 64 KiB combined key/value bytes.
- The application supplies a non-empty `PythonTopologyPolicy.allowed_module_prefixes` whenever any Python behavior is present. TOML cannot widen the allowlist.
- Imports use absolute `importlib.import_module()` only. Do not accept paths, URLs, archives, entry points, evaluated expressions, relative names, or `sys.path` mutation.
- Python topology is TOML-only, startup-only, local-process-only, and not hot reload, binary topology, or remote placement.
- Existing plain C++ topology and imperative Python APIs remain source compatible.
- C++ topology preparation performs no thread, listener, actor, name, or runtime-config mutation.
- Python import, class inspection, constructor, behavior, actor object, `on_start()`, `on_stop()`, manifest, and traceback handling run only on the dedicated asyncio thread.
- Scheduler, network, event-loop, and Python runtime threads never wait for Python topology readiness. Only the application startup worker may wait.
- Native provider, transaction, blueprint, journal, and queues contain no `PyObject*`, borrowed Python memory, Python callbacks, stack pointers, RTTI, exceptions, or public `std::function`.
- All native binding translation units compile with `-fno-exceptions -fno-rtti`; `Python.h` remains confined to `bindings/python/native/src/python_capi/`.
- Internally unpublished actors have IDs/mailboxes/contexts but no topology names, no external ingress, no committed snapshot entry, and no `SystemInit`.
- Topology names commit atomically only after every actor is ready. `SystemInit` follows complete name commit.
- Any parse, policy, import, class, constructor, behavior, start, capacity, timeout, later-actor, name, or `SystemInit` failure rolls the whole transaction back in reverse order.
- Imported modules remain in `sys.modules` after rollback; all binding-owned classes, factory records, actors, runners, futures, names, bridges, scheduler registrations, and notifier callbacks are released.
- Primary failure is preserved; secondary rollback failures accumulate bounded rollback bits and logs.
- `topology_start_timeout_ms` defaults to 30,000 and accepts 100 through 300,000 inclusive. It is a deadlock guard per actor, not a test synchronization mechanism.
- Tests use explicit gates, fake ports, injected completions, paused workers, and condition waits. Sleeps are never proof of progress.
- Full configure/build/test and four-platform installed-wheel topology smoke are required before Phase 1E is marked implemented.

## File Structure

### Native topology contracts and preparation

- Create: `bindings/python/native/include/hpactor/python/python_topology_types.hpp`
- Create: `bindings/python/native/src/python_topology_types.cpp`
- Create: `bindings/python/native/include/hpactor/python/python_topology_preparer.hpp`
- Create: `bindings/python/native/src/python_topology_preparer.cpp`
- Modify: `include/hpactor/config/python_binding_config.hpp`
- Modify: `src/config/parsers/python_binding_config_parser.cpp`
- Modify: `src/config/parsers/topology_config_parser.cpp`
- Modify: `include/hpactor/runtime/runtime_blueprint.hpp`
- Modify: `include/hpactor/runtime/runtime_blueprint_builder.hpp`
- Modify: `src/runtime/runtime_blueprint_builder.cpp`

### Transactional actor publication

- Create: `include/hpactor/runtime/actor_spawn_lease.hpp`
- Create: `src/runtime/actor_spawn_lease.cpp`
- Modify: `include/hpactor/runtime/actor_spawner.hpp`
- Modify: `src/runtime/actor_spawner.cpp`
- Modify: `include/hpactor/actor/system/actor_directory.hpp`
- Modify: `src/actor/system/actor_directory.cpp`
- Create: `include/hpactor/runtime/configured_actor_provider.hpp`
- Create: `include/hpactor/runtime/topology_bootstrap_transaction.hpp`
- Create: `src/runtime/topology_bootstrap_transaction.cpp`
- Modify: `include/hpactor/actor/system/actor_system.hpp`
- Modify: `src/actor/system/actor_system.cpp`

### Python native provider and Stable-ABI module

- Create: `bindings/python/native/include/hpactor/python/python_topology_provider.hpp`
- Create: `bindings/python/native/src/python_topology_provider.cpp`
- Modify: `bindings/python/native/include/hpactor/python/python_bridge_types.hpp`
- Modify: `bindings/python/native/include/hpactor/python/python_runtime_snapshot.hpp`
- Modify: `bindings/python/native/include/hpactor/python/python_observability.hpp`
- Modify: `bindings/python/native/src/python_observability.cpp`
- Modify: `bindings/python/native/include/hpactor/python/python_native_system.hpp`
- Modify: `bindings/python/native/src/python_native_system.cpp`
- Modify: `bindings/python/native/src/python_cli_commands.cpp`
- Modify: `bindings/python/native/src/python_capi/native_system_type.cpp`
- Modify: `bindings/python/native/src/python_capi/conversions.hpp`
- Modify: `bindings/python/native/src/python_capi/conversions.cpp`
- Modify: `bindings/python/native/CMakeLists.txt`

### Pure-Python API and runtime

- Create: `bindings/python/hpactor/_topology.py`
- Modify: `bindings/python/hpactor/_errors.py`
- Modify: `bindings/python/hpactor/_runtime.py`
- Modify: `bindings/python/hpactor/_system.py`
- Modify: `bindings/python/hpactor/__init__.py`

### Tests, manual, and acceptance evidence

- Create: `tests/unit/python/test_python_topology_types.cpp`
- Create: `tests/unit/python/test_python_topology_preparer.cpp`
- Create: `tests/unit/runtime/test_actor_spawn_lease.cpp`
- Create: `tests/unit/runtime/test_topology_bootstrap_transaction.cpp`
- Create: `tests/unit/python/test_python_topology_provider.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`
- Modify: `tests/unit/runtime/CMakeLists.txt`
- Create: `bindings/python/tests/unit/test_topology_policy.py`
- Create: `bindings/python/tests/unit/test_topology_manifest.py`
- Create: `bindings/python/tests/unit/test_topology_system.py`
- Create: `bindings/python/tests/fixtures/topology_app/__init__.py`
- Create: `bindings/python/tests/fixtures/topology_app/actors.py`
- Create: `bindings/python/tests/data/topology_python.toml`
- Create: `bindings/python/tests/data/topology_import_failure.toml`
- Create: `bindings/python/tests/data/topology_start_failure.toml`
- Create: `bindings/python/tests/integration/test_topology.py`
- Create: `tests/integration/python/test_python_topology_transaction.cpp`
- Modify: `tests/integration/python/CMakeLists.txt`
- Modify: `bindings/python/tests/CMakeLists.txt`
- Modify: `tests/architecture/CMakeLists.txt`
- Create: `docs/manual/python/topology.rst`
- Modify: `docs/manual/python/index.rst`
- Modify: `docs/manual/python/deployment.rst`
- Modify: `bindings/python/examples/README.md`
- Create: `bindings/python/examples/topology.py`
- Create: `bindings/python/examples/topology.toml`
- Modify: `bindings/python/tests/wheel/test_reliability_smoke.py`
- Modify: `CLAUDE_MEMORY.md`
- Modify: `docs/superpowers/specs/2026-07-03-python-language-binding-design.md`
- Modify: `docs/superpowers/specs/2026-07-06-python-binding-phase1e-declarative-topology-design.md`

---

### Task 1: Add exact Python topology syntax, argument, and timeout contracts

**Files:**
- Create: `bindings/python/native/include/hpactor/python/python_topology_types.hpp`
- Create: `bindings/python/native/src/python_topology_types.cpp`
- Modify: `include/hpactor/config/python_binding_config.hpp`
- Modify: `src/config/parsers/python_binding_config_parser.cpp`
- Create: `tests/unit/python/test_python_topology_types.cpp`
- Modify: `tests/unit/python/test_python_binding_config.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`
- Modify: `bindings/python/native/CMakeLists.txt`
- Modify: `tests/integration/config/test_toml_parser.cpp`

**Interfaces:**
- Consumes: `config::ActorDef`, Phase 1C `config::PythonBindingConfig`, `result<T>`, and string-valued topology arguments.
- Produces: `ConfiguredActorKind`, `PythonTopologyPhase`, `PythonTopologyErrorInfo`, `PythonBehaviorRef`, `PreparedActorSpec`, `parse_python_behavior_ref()`, `validate_python_actor_args()`, `fingerprint_python_actor_args()`, and validated `topology_start_timeout_ms`.

- [ ] **Step 1: Write failing behavior-reference and argument-bound tests**

```cpp
TEST(PythonTopologyTypesTest, ParsesApprovedBehaviorReference) {
    auto parsed = python::parse_python_behavior_ref(
        "python:my_app.workers:Workers.Ingest");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().module, "my_app.workers");
    EXPECT_EQ(parsed.value().qualname, "Workers.Ingest");
}

TEST(PythonTopologyTypesTest, RejectsPathRelativeAndLocalReferences) {
    for (std::string_view value : {
             "python:.actors:Echo", "python:/tmp/actors.py:Echo",
             "python:pkg.actors:<locals>.Echo", "python:pkg:Echo:Extra",
             "python:pkg-name:Echo", "python:pkg:", "python::Echo"}) {
        EXPECT_FALSE(python::parse_python_behavior_ref(value).has_value())
            << value;
    }
}

TEST(PythonTopologyTypesTest, RejectsArgumentBudgetViolations) {
    config::ActorDef def;
    def.id = "echo";
    def.behavior = "python:pkg.actors:Echo";
    def.args["__hpactor_token"] = "secret";
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());
    def.args.clear();
    def.args["not-valid"] = "value";
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());
}
```

Add exact boundary cases for 255-byte module/qualname, 518-byte behavior,
128-byte keys, 4096-byte values, 128 arguments, and 64 KiB combined bytes.
Test one byte over every limit and prove argument fingerprints are independent
of `unordered_map` iteration order.

- [ ] **Step 2: Run the focused tests to verify RED**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
  -DENABLE_PYTHON_BINDINGS=ON
ninja -C build test_unit_python_binding test_integration_config
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonTopologyTypesTest.*:PythonBindingConfigTest.*Topology*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='TomlParserTest.*ActorArgs*'
```

Expected: compilation fails because topology types and the timeout field do not
exist.

- [ ] **Step 3: Define the exact native value types**

```cpp
enum class ConfiguredActorKind : uint8_t { Cpp = 0, Python = 1 };

enum class PythonTopologyPhase : uint8_t {
    Idle = 0,
    Parse = 1,
    Policy = 2,
    Import = 3,
    ClassResolution = 4,
    ClassValidation = 5,
    ConstructorBinding = 6,
    NativePrepare = 7,
    ActorStart = 8,
    Commit = 9,
    Rollback = 10,
};

struct PythonTopologyErrorInfo final {
    PythonTopologyPhase phase{PythonTopologyPhase::Idle};
    std::string actor_id;
    std::string behavior;
    uint32_t error_code{0};
    std::string detail;
    uint32_t rollback_error_bits{0};
};

struct PythonBehaviorRef final {
    std::string module;
    std::string qualname;
};

struct PreparedActorSpec final {
    size_t topology_index{0};
    ConfiguredActorKind kind{ConfiguredActorKind::Cpp};
    std::optional<PythonBehaviorRef> python;
    uint64_t args_fingerprint{0};
    uint64_t factory_token{0};
};

result<PythonBehaviorRef>
parse_python_behavior_ref(std::string_view behavior) noexcept;
result<void> validate_python_actor_args(const config::ActorDef& def) noexcept;
uint64_t fingerprint_python_actor_args(const config::ActorDef& def) noexcept;
```

`PythonTopologyPreparer` calls the parser only when a behavior starts with the
exact `python:` prefix; every other value is classified as C++. A prefixed but
malformed value returns `errors::invalid_argument`. Use byte-wise ASCII
validation and FNV-1a over sorted, length-prefixed key/value bytes. Do not use
regex, locale APIs, exceptions, or RTTI.

- [ ] **Step 4: Add and validate the topology start timeout**

Add `uint32_t topology_start_timeout_ms{30000}` to
`PythonBindingConfig`. Parse it in the existing self-registering
`python_binding_config_parser.cpp`; accept 100 through 300,000 inclusive and
return the existing configuration error path outside that range. Do not edit
`parse_file_data()`.

In `topology_config_parser.cpp`, make the existing actor-argument parser reject
array, table, date/time, and other non-scalar values for every actor instead of
silently dropping them. Preserve string/integer/float/Boolean canonicalization.
Capture the first invalid `actor.args.<key>` in the document parser callback and
return `TomlParseContext::fail()` after the table-array walk. Add an integration
parser test proving a Python actor with `args = { values = [1, 2] }` fails and
the same actor with four scalar kinds produces four canonical strings.

- [ ] **Step 5: Run the focused tests to verify GREEN**

```bash
ninja -C build test_unit_python_binding test_integration_config
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonTopologyTypesTest.*:PythonBindingConfigTest.*Topology*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='TomlParserTest.*ActorArgs*'
```

Expected: grammar, every exact bound, deterministic fingerprint, and timeout
default/min/max/rejection tests pass, and non-scalar actor args are rejected.

- [ ] **Step 6: Commit topology value contracts**

```bash
git add bindings/python/native/include/hpactor/python/python_topology_types.hpp \
  bindings/python/native/src/python_topology_types.cpp \
  bindings/python/native/CMakeLists.txt \
  include/hpactor/config/python_binding_config.hpp \
  src/config/parsers/python_binding_config_parser.cpp \
  src/config/parsers/topology_config_parser.cpp \
  tests/unit/python \
  tests/integration/config/test_toml_parser.cpp
git commit -m "feat: define Python topology contracts"
```

### Task 2: Prepare immutable topology and effective runtime fingerprints

**Files:**
- Create: `bindings/python/native/include/hpactor/python/python_topology_preparer.hpp`
- Create: `bindings/python/native/src/python_topology_preparer.cpp`
- Modify: `include/hpactor/runtime/runtime_blueprint.hpp`
- Modify: `include/hpactor/runtime/runtime_blueprint_builder.hpp`
- Modify: `src/runtime/runtime_blueprint_builder.cpp`
- Modify: `bindings/python/native/include/hpactor/python/python_native_system.hpp`
- Modify: `bindings/python/native/src/python_native_system.cpp`
- Create: `tests/unit/python/test_python_topology_preparer.cpp`
- Modify: `tests/unit/runtime/test_runtime_blueprint.cpp`

**Interfaces:**
- Consumes: `TomlParser::parse()`, `TopologyModel`, `ActorFactoryRegistry`, Task 1 contracts, `Config`, and application policy fingerprint.
- Produces: immutable `ParsedTopologyPlan`, immutable token-bound `PreparedTopology`, `RuntimeBlueprintBuilder::from_config_and_topology()`, and native prepare/bind methods with no side effects.

- [ ] **Step 1: Write failing preparation purity and classification tests**

```cpp
TEST(PythonTopologyPreparerTest, ClassifiesMixedTopologyWithoutStartingRuntime) {
    CountingRuntimeProbe probe;
    auto parsed = PythonTopologyPreparer::parse(
        data_path("mixed_python_cpp.toml"), cpp_registry(), probe.ports());
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed.value().actors().size(), 2u);
    EXPECT_EQ(parsed.value().actors()[0].kind, ConfiguredActorKind::Python);
    EXPECT_EQ(parsed.value().actors()[1].kind, ConfiguredActorKind::Cpp);
    EXPECT_EQ(probe.thread_starts, 0u);
    EXPECT_EQ(probe.actor_spawns, 0u);
    EXPECT_EQ(probe.name_publications, 0u);
}

TEST(PythonTopologyPreparerTest, BindingRequiresEveryExactPythonSpec) {
    auto parsed = parse_one_python_actor();
    std::array<FactoryTokenBinding, 1> bindings{{
        {0, 7, parsed.actors()[0].args_fingerprint}}};
    auto prepared = parsed.bind_manifest(bindings, 0x1122334455667788ULL);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_NE(prepared.value().effective_fingerprint(), 0u);
    bindings[0].args_fingerprint ^= 1;
    EXPECT_FALSE(parsed.bind_manifest(bindings, 1).has_value());
}
```

Also test missing/duplicate/zero tokens, reordered indices, unknown C++
behaviors, exact C++/`python:` collision, malformed Python references, actor
count above `max_actor_bindings`, and binary magic rejection.

- [ ] **Step 2: Run preparation tests to verify RED**

```bash
ninja -C build test_unit_python_binding test_unit_runtime
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonTopologyPreparerTest.*'
```

Expected: compilation fails because the preparer and immutable plans do not
exist.

- [ ] **Step 3: Implement immutable parsed and prepared plans**

```cpp
struct FactoryTokenBinding final {
    size_t topology_index{0};
    uint64_t factory_token{0};
    uint64_t args_fingerprint{0};
};

class ParsedTopologyPlan final {
  public:
    const config::TopologyModel& model() const noexcept;
    std::span<const PreparedActorSpec> actors() const noexcept;
    uint64_t topology_fingerprint() const noexcept;
    result<PreparedTopology>
    bind_manifest(std::span<const FactoryTokenBinding> bindings,
                  uint64_t policy_fingerprint) const noexcept;
};

class PreparedTopology final {
  public:
    const config::TopologyModel& model() const noexcept;
    std::span<const PreparedActorSpec> actors() const noexcept;
    uint64_t effective_fingerprint() const noexcept;
};
```

Construct through private constructors owned by `PythonTopologyPreparer`.
Canonicalize topology fingerprint input by actor order and sorted argument
keys. `bind_manifest()` copies the model/specs and returns a new immutable
object; it never mutates `ParsedTopologyPlan`.

- [ ] **Step 4: Extend runtime blueprint construction**

Add:

```cpp
static result<RuntimeBlueprint> from_config_and_topology(
    const Config& config, const config::TopologyModel& topology,
    uint64_t extension_fingerprint) noexcept;
```

Populate `ConfiguredActorSpec` in model order and include every actor ID,
behavior, sorted argument key/value bytes, and the extension fingerprint in the
existing deterministic blueprint hash. Keep `from_config()` unchanged.

- [ ] **Step 5: Add side-effect-free native prepare and bind storage**

Add to `PythonNativeSystem`:

```cpp
result<std::vector<PythonTopologyDescriptor>>
prepare_topology(std::string_view path) noexcept;
result<uint64_t> bind_topology_manifest(
    std::span<const FactoryTokenBinding> bindings,
    uint64_t policy_fingerprint) noexcept;
```

`PythonTopologyDescriptor` contains topology index, actor ID, original behavior,
module, qualname, sorted argument pairs, and argument fingerprint. Store one
optional parsed plan, one optional prepared plan, and reject prepare/bind after
runtime start. On failure store one bounded `PythonTopologyErrorInfo` rather
than requiring callers to parse text. `create()` and preparation use
`RuntimeBuilder` to keep the native system in `Built`; do not start legacy
constructor services.

- [ ] **Step 6: Run preparation and blueprint tests to verify GREEN**

```bash
ninja -C build test_unit_python_binding test_unit_runtime
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonTopologyPreparerTest.*'
./build/tests/unit/runtime/test_unit_runtime \
  --gtest_filter='RuntimeBlueprintTest.*Topology*'
```

Expected: preparation is mutation-free, manifest binding is exact, binary
Python topology is rejected, and effective fingerprints change for behavior,
args, actor order, or policy changes.

- [ ] **Step 7: Commit immutable topology preparation**

```bash
git add bindings/python/native include/hpactor/runtime/runtime_blueprint.hpp \
  include/hpactor/runtime/runtime_blueprint_builder.hpp \
  src/runtime/runtime_blueprint_builder.cpp tests/unit/python \
  tests/unit/runtime/test_runtime_blueprint.cpp
git commit -m "feat: prepare Python topology before startup"
```

### Task 3: Add internally unpublished actor leases and atomic name commit

**Files:**
- Create: `include/hpactor/runtime/actor_spawn_lease.hpp`
- Create: `src/runtime/actor_spawn_lease.cpp`
- Modify: `include/hpactor/runtime/actor_spawner.hpp`
- Modify: `src/runtime/actor_spawner.cpp`
- Modify: `include/hpactor/actor/system/actor_directory.hpp`
- Modify: `src/actor/system/actor_directory.cpp`
- Create: `tests/unit/runtime/test_actor_spawn_lease.cpp`
- Modify: `tests/unit/actor/test_actor_directory.cpp`
- Modify: `tests/unit/runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: `ActorSpawner::adopt()`, `SpawnSpec`, `ActorDirectory`, scheduler placement, actor lifecycle, mailbox ownership, and registered names.
- Produces: move-only `ActorSpawnLease`, `ActorSpawner::adopt_unpublished()`, `ActorDirectory::register_names_atomically()`, explicit lease commit, and idempotent complete rollback.

- [ ] **Step 1: Write failing unpublished and atomic-name tests**

```cpp
TEST(ActorSpawnLeaseTest, UncommittedLeaseIsNotNameVisibleAndRollsBack) {
    Fixture f;
    auto lease = f.spawner.adopt_unpublished(
        std::make_shared<TestActor>(), f.spec("echo"));
    ASSERT_TRUE(lease.has_value());
    ActorId id = lease.value().actor().id();
    EXPECT_TRUE(f.directory.find(id).has_value());
    EXPECT_FALSE(f.directory.resolve_name("echo").has_value());
    EXPECT_TRUE(lease.value().rollback().has_value());
    EXPECT_FALSE(f.directory.find(id).has_value());
    EXPECT_EQ(f.scheduler.unregister_calls(id), 1u);
}

TEST(ActorDirectoryTest, AtomicNameRegistrationPublishesAllOrNone) {
    DirectoryFixture f;
    auto a = f.publish_unnamed();
    auto b = f.publish_unnamed();
    ASSERT_TRUE(f.directory.register_name("taken", a.id()));
    std::array<NamedActor, 2> batch{{{"new", a.id()}, {"taken", b.id()}}};
    EXPECT_EQ(f.directory.register_names_atomically(batch),
              NameBatchStatus::DuplicateName);
    EXPECT_FALSE(f.directory.resolve_name("new").has_value());
}
```

Add lease move, double rollback, rollback after partial scheduler registration,
commit transfer, duplicate ID, duplicate name within one batch, missing actor,
and concurrent resolver visibility tests.

- [ ] **Step 2: Run actor runtime tests to verify RED**

```bash
ninja -C build test_unit_runtime test_unit_actor
./build/tests/unit/runtime/test_unit_runtime \
  --gtest_filter='ActorSpawnLeaseTest.*'
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorDirectoryTest.*Atomic*'
```

Expected: compilation fails because unpublished leases and batch name commit do
not exist.

- [ ] **Step 3: Implement the move-only actor lease**

```cpp
class ActorSpawnLease final {
  public:
    ActorSpawnLease(ActorSpawnLease&&) noexcept;
    ActorSpawnLease& operator=(ActorSpawnLease&&) noexcept;
    ~ActorSpawnLease();

    const Actor& actor() const noexcept;
    result<void> commit() noexcept;
    result<void> rollback() noexcept;

    ActorSpawnLease(const ActorSpawnLease&) = delete;
    ActorSpawnLease& operator=(const ActorSpawnLease&) = delete;
};
```

The lease journal records which adoption steps completed: directory
publication, lifecycle activation, dedicated scheduler registration, metrics,
and logging. Rollback reverses only completed steps and is idempotent. A live
uncommitted destructor invokes rollback. `commit()` transfers actor ownership
to `ActorRuntime` and disables destructor rollback.

- [ ] **Step 4: Split adoption into unpublished and compatibility paths**

Add:

```cpp
result<ActorSpawnLease>
ActorSpawner::adopt_unpublished(std::shared_ptr<AbstractActor> actor,
                                const SpawnSpec& spec) noexcept;
```

Keep `adopt()` source-compatible by calling `adopt_unpublished()`, atomically
registering `spec.registered_name` when non-empty, committing the lease, and
returning its actor. Preserve existing telemetry exactly once.

- [ ] **Step 5: Implement all-or-none name registration**

Define `NamedActor { std::string_view name; ActorId actor; }` and
`NameBatchStatus { Published, DuplicateName, DuplicateActor, MissingActor,
InvalidName }`. Under one directory write lock, validate the complete batch
before inserting any name. Readers see either the old map or the complete new
map, never a prefix.

- [ ] **Step 6: Run lease, directory, and existing spawn tests**

```bash
ninja -C build test_unit_runtime test_unit_actor
./build/tests/unit/runtime/test_unit_runtime \
  --gtest_filter='ActorSpawnLeaseTest.*:ActorSpawnerTest.*'
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorDirectoryTest.*:ActorSystemSpawnTest.*'
```

Expected: unpublished visibility, exact rollback, atomic names, and all
existing imperative spawn tests pass.

- [ ] **Step 7: Commit unpublished actor leases**

```bash
git add include/hpactor/runtime/actor_spawn_lease.hpp \
  include/hpactor/runtime/actor_spawner.hpp \
  include/hpactor/actor/system/actor_directory.hpp \
  src/runtime/actor_spawn_lease.cpp src/runtime/actor_spawner.cpp \
  src/actor/system/actor_directory.cpp tests/unit/runtime \
  tests/unit/actor/test_actor_directory.cpp
git commit -m "feat: add transactional actor spawn leases"
```

### Task 4: Build the generic mixed-topology bootstrap transaction

**Files:**
- Create: `include/hpactor/runtime/configured_actor_provider.hpp`
- Create: `include/hpactor/runtime/topology_bootstrap_transaction.hpp`
- Create: `src/runtime/topology_bootstrap_transaction.cpp`
- Modify: `include/hpactor/actor/system/actor_system.hpp`
- Modify: `src/actor/system/actor_system.cpp`
- Modify: `src/CMakeLists.txt`
- Create: `tests/unit/runtime/test_topology_bootstrap_transaction.cpp`
- Modify: `tests/unit/runtime/CMakeLists.txt`
- Modify: `tests/system/test_system_topology_bootstrap.cpp`

**Interfaces:**
- Consumes: the `TopologyModel`, effective fingerprint, and generic actor-plan projection from `PreparedTopology`; `ActorSpawnLease`, `ActorFactoryRegistry`, `ActorSpawner`, `ActorDirectory`, delivery results, and system actor address.
- Produces: fixed `ConfiguredActorProviderPort`, `TopologyBootstrapTransaction::execute()`, reverse journal rollback, atomic topology commit, and a dedicated prepared-topology entry point that leaves ordinary C++ `load_topology()` unchanged.

- [ ] **Step 1: Write a failing mixed-provider transaction test**

```cpp
TEST(TopologyBootstrapTransactionTest, CommitsNamesAfterEveryActorIsReady) {
    TransactionFixture f;
    f.provider.ready_gate.close();
    auto future = f.run_async(f.mixed_topology());
    f.provider.wait_until_spawned(2);
    EXPECT_FALSE(f.directory.resolve_name("python-echo").has_value());
    EXPECT_FALSE(f.directory.resolve_name("cpp-audit").has_value());
    f.provider.ready_gate.open();
    ASSERT_TRUE(future.get().has_value());
    EXPECT_TRUE(f.directory.resolve_name("python-echo").has_value());
    EXPECT_TRUE(f.directory.resolve_name("cpp-audit").has_value());
    EXPECT_EQ(f.system_init_order,
              (std::vector<std::string>{"python-echo", "cpp-audit"}));
}

TEST(TopologyBootstrapTransactionTest, LaterFailureRollsBackReverseOrder) {
    TransactionFixture f;
    f.provider.fail_ready_at = 2;
    auto result = f.execute(f.three_actor_topology());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(f.provider.rollback_order,
              (std::vector<size_t>{1, 0}));
    EXPECT_EQ(f.directory.size(), f.baseline_directory_size);
    EXPECT_EQ(f.names(), f.baseline_names);
}
```

Add failures for provider mismatch, factory construction, lease adoption,
bounded readiness, timeout, atomic-name collision, and rejected `SystemInit`.
Inject rollback failures and assert primary error preservation plus stable
rollback-bit positions.

- [ ] **Step 2: Run the transaction test to verify RED**

```bash
ninja -C build test_unit_runtime
./build/tests/unit/runtime/test_unit_runtime \
  --gtest_filter='TopologyBootstrapTransactionTest.*'
```

Expected: compilation fails because the provider port and transaction do not
exist.

- [ ] **Step 3: Define the provider port and outcomes**

```cpp
enum class ConfiguredActorProviderKind : uint8_t {
    BuiltinCpp = 0,
    External = 1,
};

struct ConfiguredActorPlan final {
    size_t topology_index{0};
    ConfiguredActorProviderKind provider{ConfiguredActorProviderKind::BuiltinCpp};
    uint64_t provider_token{0};
    uint64_t args_fingerprint{0};
};

struct ConfiguredActorProviderPort final {
    void* context{nullptr};
    bool (*matches)(void*, const ConfiguredActorPlan&) noexcept {nullptr};
    result<ActorSpawnLease> (*spawn_unpublished)(
        void*, const config::ActorDef&,
        const ConfiguredActorPlan&) noexcept {nullptr};
    result<void> (*await_ready)(
        void*, ActorId, const ConfiguredActorPlan&,
        std::chrono::milliseconds) noexcept {nullptr};
    result<void> (*rollback_actor)(
        void*, ActorId, const ConfiguredActorPlan&) noexcept {nullptr};
};

struct TopologyBootstrapResult final {
    uint64_t fingerprint{0};
    uint32_t actor_count{0};
    uint32_t rollback_error_bits{0};
};
```

The port is optional. `BuiltinCpp` specs use the exact C++ factory registry;
`External` specs require a matching port and a nonzero provider token. Reject a
port that matches a built-in spec or fails to match an external spec. This core
header has no dependency on binding or Python topology types.

- [ ] **Step 4: Implement the bounded journal and transaction algorithm**

Pre-size one journal entry per actor before mutation. For each actor in model
order: choose provider, construct/adopt internally unpublished, wait for ready,
and append the live lease. On complete readiness, call
`register_names_atomically()` once, then deliver `SystemInit` with the full
delivery pipeline and require an accepted result for every actor. Commit every
lease only after all initialization deliveries are accepted.

On failure: remove any committed batch names, call provider rollback for each
started actor in reverse order, roll back its lease, resolve remaining records
as aborted, and retain the first error. Assign rollback bits by stable phase:
name removal `1<<0`, provider cleanup `1<<1`, actor lease `1<<2`, token abort
`1<<3`, runtime-stage cleanup `1<<4`.

- [ ] **Step 5: Add the dedicated prepared-topology entry point**

Add an internal binding-facing entry point:

```cpp
result<TopologyBootstrapResult> bootstrap_prepared_topology(
    const config::TopologyModel& model,
    std::span<const ConfiguredActorPlan> specs,
    uint64_t effective_fingerprint,
    ConfiguredActorProviderPort provider,
    std::chrono::milliseconds actor_start_timeout) noexcept;
```

It applies the already validated model and runs the transaction. Restrict it to
the runtime composition/binding seam rather than general application code.
Keep ordinary `ActorSystem::load_topology()` implementation and success/failure
semantics unchanged, and never install the Python provider in ordinary C++
construction.

- [ ] **Step 6: Run transaction and C++ topology compatibility tests**

```bash
ninja -C build test_unit_runtime test_system
./build/tests/unit/runtime/test_unit_runtime \
  --gtest_filter='TopologyBootstrapTransactionTest.*'
./build/tests/system/test_system \
  --gtest_filter='*TopologyBootstrap*'
```

Expected: all transaction fault points roll back exactly; existing C++
topology success tests pass unchanged.

- [ ] **Step 7: Commit the generic transaction**

```bash
git add include/hpactor/runtime/configured_actor_provider.hpp \
  include/hpactor/runtime/topology_bootstrap_transaction.hpp \
  include/hpactor/actor/system/actor_system.hpp \
  src/runtime/topology_bootstrap_transaction.cpp \
  src/actor/system/actor_system.cpp src/CMakeLists.txt \
  tests/unit/runtime tests/system/test_system_topology_bootstrap.cpp
git commit -m "feat: transact configured topology startup"
```

### Task 5: Implement the value-only native Python topology provider

**Files:**
- Create: `bindings/python/native/include/hpactor/python/python_topology_provider.hpp`
- Create: `bindings/python/native/src/python_topology_provider.cpp`
- Modify: `bindings/python/native/include/hpactor/python/python_bridge_types.hpp`
- Modify: `bindings/python/native/include/hpactor/python/python_runtime_snapshot.hpp`
- Modify: `bindings/python/native/include/hpactor/python/python_runtime.hpp`
- Modify: `bindings/python/native/src/python_runtime.cpp`
- Modify: `bindings/python/native/include/hpactor/python/python_observability.hpp`
- Modify: `bindings/python/native/src/python_observability.cpp`
- Modify: `bindings/python/native/include/hpactor/python/python_native_system.hpp`
- Modify: `bindings/python/native/src/python_native_system.cpp`
- Modify: `bindings/python/native/src/python_cli_commands.cpp`
- Modify: `bindings/python/native/CMakeLists.txt`
- Create: `tests/unit/python/test_python_topology_provider.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`

**Interfaces:**
- Consumes: prepared factory tokens, Phase 1A queues/generations, Phase 1B bridge spawn, Phase 1C snapshots/shutdown, Task 4 provider port, and startup timeout.
- Produces: append-only topology dispatch/completion kinds, bounded `PythonTopologyReadyTable`, `PythonTopologyProvider::port()`, native start/complete/rollback methods, topology snapshot counters, exact topology metrics, and `/python status` fields.

- [ ] **Step 1: Write failing provider ownership and waiting tests**

```cpp
TEST(PythonTopologyProviderTest, SpawnQueuesValueOnlyInstallAndWaitsOffScheduler) {
    ProviderFixture f;
    auto spawned = f.provider.spawn_unpublished(f.python_def(), f.spec(11));
    ASSERT_TRUE(spawned.has_value());
    auto install = f.runtime.pop_dispatch();
    ASSERT_EQ(install.kind, PythonDispatchKind::TopologyInstall);
    EXPECT_EQ(install.factory_token, 11u);
    EXPECT_EQ(install.actor_generation, spawned.value().actor().incarnation());

    auto waiter = f.wait_ready_async(spawned.value().actor().id(), f.spec(11));
    f.provider.complete(11, TopologyActorOutcome::Ready, 0, "");
    EXPECT_TRUE(waiter.get().has_value());
}

TEST(PythonTopologyProviderTest, RejectedInstallAndStaleCompletionFailSafely) {
    ProviderFixture f(/*dispatch_capacity=*/1);
    f.runtime.fill_dispatch_queue();
    EXPECT_FALSE(f.provider.spawn_unpublished(f.python_def(), f.spec(7))
                     .has_value());
    EXPECT_FALSE(f.provider.complete_for_generation(
        7, f.system_generation() - 1, TopologyActorOutcome::Ready, 0, ""));
}
```

Add capacity, duplicate token, timeout, cancellation, detail truncation,
generation fencing, rollback-before-ready, rollback-after-ready, double
rollback, and no-self-wait tests.

- [ ] **Step 2: Run provider tests to verify RED**

```bash
ninja -C build test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonTopologyProviderTest.*'
```

Expected: compilation fails because topology provider records and readiness
table do not exist.

- [ ] **Step 3: Append exact value-only topology records**

Append without renumbering existing values:

```cpp
enum class PythonDispatchKind : uint8_t {
    // existing Phase 1B/1C values remain unchanged
    TopologyInstall = 4,
    TopologyRollback = 5,
};

enum class TopologyActorOutcome : uint8_t {
    Ready = 0,
    ConstructorFailed = 1,
    BehaviorFailed = 2,
    StartFailed = 3,
    RolledBack = 4,
    Cancelled = 5,
};

```

Extend dispatch records with topology index, factory token, and argument
fingerprint. Extend native snapshots with topology fingerprint,
configured/prepared/started/committed/rolled-back counts, current phase,
failure phase, and bounded last actor ID. Do not store module/class objects or
argument values in snapshots.

Extend Phase 1C observability with these exact metrics:

```text
hpactor_python_topology_preflight_total{outcome}
hpactor_python_topology_import_total{outcome}
hpactor_python_topology_actor_start_total{outcome}
hpactor_python_topology_rollback_total{outcome}
hpactor_python_topology_start_duration_seconds
hpactor_python_topology_actor_count
```

Use bounded enum-valued outcome labels only. Add the snapshot fields to
`/python status`; never expose constructor arguments or traceback text.

- [ ] **Step 4: Implement the bounded readiness table**

Allocate at most `max_actor_bindings` entries before topology mutation. Key by
factory token and store system generation, actor generation, outcome, bounded
detail, mutex, and condition variable. Only the startup worker calls
`wait_ready()`; reject the Python runtime thread, scheduler workers, event-loop
thread, and native gateway thread using explicit thread-role checks.

`complete()` validates token and both generations, transitions once, signals
the waiter, and counts stale/duplicate completions. The per-actor timeout uses
`topology_start_timeout_ms` and returns a typed startup error.

- [ ] **Step 5: Bind the provider to native lifecycle**

Add:

```cpp
result<void> start_prepared_topology() noexcept;
result<void> complete_topology_actor(
    uint64_t factory_token, uint64_t system_generation,
    uint64_t actor_generation, TopologyActorOutcome outcome,
    uint32_t error_code, std::string_view detail) noexcept;
result<void> record_topology_preflight(
    PythonTopologyPhase phase, bool success) noexcept;
std::optional<PythonTopologyErrorInfo> last_topology_error() const noexcept;
```

Start scheduler/actor runtime, gateway, and Python notifier stages before the
transaction; keep network/application ingress and readiness closed. Execute
Task 4 with `PythonTopologyProvider::port()`. On success continue ingress and
ready stages. On failure let the topology journal and `RuntimeCoordinator`
roll back in order, join the Python runtime through the existing Phase 1C
shutdown adapter, and leave state `Failed`.

Update `last_topology_error` at every actor-start, commit, and rollback failure.
Never overwrite its primary phase/code/detail with a rollback error; merge only
the stable rollback bitmask.

Translate each bound `PreparedActorSpec` into a core `ConfiguredActorPlan`:
C++ becomes `BuiltinCpp` with token zero; Python becomes `External` with its
factory token and argument fingerprint. The generic runtime layer never sees a
module name, class name, Python enum, or binding header.

- [ ] **Step 6: Run provider and lifecycle tests to verify GREEN**

```bash
ninja -C build test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonTopologyProviderTest.*:PythonNativeSystemTest.*Topology*'
```

Expected: value-only install, exact wait-role rules, generation fencing,
timeout, reverse rollback, counters, and failed readiness pass.

- [ ] **Step 7: Commit the native Python provider**

```bash
git add bindings/python/native tests/unit/python
git commit -m "feat: add native Python topology provider"
```

### Task 6: Expose topology preparation and startup through the limited API

**Files:**
- Modify: `bindings/python/native/src/python_capi/native_system_type.cpp`
- Modify: `bindings/python/native/src/python_capi/conversions.hpp`
- Modify: `bindings/python/native/src/python_capi/conversions.cpp`
- Modify: `bindings/python/tests/unit/test_native_module.py`
- Modify: `tests/architecture/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 native descriptors/bindings and Task 5 start/completion API.
- Produces: Stable-ABI `NativeSystem.prepare_topology()`, `bind_topology_manifest()`, `start_prepared_topology()`, `complete_topology_actor()`, and `resolve_name()` with checked immutable tuple schemas.

- [ ] **Step 1: Write failing module schema tests**

```python
class NativeTopologyModuleTest(unittest.TestCase):
    def test_prepare_and_bind_use_exact_value_schemas(self) -> None:
        native = _hpactor.NativeSystem({"max_actor_bindings": 64})
        descriptors = native.prepare_topology(data_path("topology_python.toml"))
        self.assertEqual(len(descriptors), 1)
        index, actor_id, behavior, module, qualname, args, fingerprint = descriptors[0]
        self.assertEqual((index, actor_id), (0, "echo"))
        self.assertEqual(behavior, "python:topology_app.actors:Echo")
        self.assertEqual((module, qualname), ("topology_app.actors", "Echo"))
        self.assertEqual(args, (("prefix", "prod"),))
        effective = native.bind_topology_manifest(
            ((index, 1, fingerprint),), 0x1234)
        self.assertGreater(effective, 0)
```

Add malformed tuple length/type/range, duplicate index/token, stale generation,
method-before-prepare, method-after-start, and binary topology rejection tests.

- [ ] **Step 2: Run native module tests to verify RED**

```bash
ninja -C build _hpactor
ctest --test-dir build -R 'PythonNativeModule' --output-on-failure
```

Expected: topology methods are absent.

- [ ] **Step 3: Implement exact limited-API methods**

Expose:

```text
prepare_topology(path: str) -> tuple[
  tuple[index, actor_id, behavior, module, qualname,
        tuple[tuple[key, value], ...], args_fingerprint], ...]
bind_topology_manifest(
  tuple[tuple[index, factory_token, args_fingerprint], ...],
  policy_fingerprint: int) -> int
start_prepared_topology() -> None
complete_topology_actor(factory_token, system_generation, actor_generation,
                        outcome, error_code, detail) -> None
record_topology_preflight(phase, success) -> None
last_topology_error() -> tuple[
  phase, actor_id_or_none, behavior_or_none,
  error_code, detail, rollback_error_bits] | None
resolve_name(name: str) -> address_tuple | None
```

Require UTF-8 strings, unsigned 64-bit integers, exact tuple lengths, sorted
argument pairs, nonzero tokens, and detail at most 4096 bytes. Native methods
still use `ValueError`, `RuntimeError`, or `MemoryError`; `last_topology_error()`
provides the stable structured phase/actor/behavior/code/detail/rollback data
that the pure-Python layer converts to `TopologyError`.

- [ ] **Step 4: Release the GIL during blocking native preparation and startup**

After converting the path, `prepare_topology()` releases the GIL around native
file parsing and restores it before creating descriptor tuples.
`start_prepared_topology()` verifies it is not called on the dedicated Python
actor thread, releases the GIL with Stable-ABI thread-state APIs, runs the
native transaction on the application startup worker, restores the GIL, and
then maps the explicit result. No other C API method may block waiting for an
actor.

- [ ] **Step 5: Extend architecture containment checks**

Add scans proving topology provider/transaction/blueprint/lease files contain
no `Python.h`, `PyObject`, `Py_BEGIN_ALLOW_THREADS`, `throw`, `catch`,
`dynamic_cast`, `typeid`, or public `std::function`. Permit GIL-release APIs
only in `native_system_type.cpp`. Assert no Python topology type appears in
ordinary `TomlParser::parse_file_data()`.

- [ ] **Step 6: Run module and architecture tests to verify GREEN**

```bash
ninja -C build _hpactor test_architecture
ctest --test-dir build \
  -R 'PythonNativeModule|PythonBindingArchitecture' --output-on-failure
```

Expected: exact schemas, invalid-state errors, GIL release, and containment
checks pass.

- [ ] **Step 7: Commit Stable-ABI topology endpoints**

```bash
git add bindings/python/native/src/python_capi \
  bindings/python/tests/unit/test_native_module.py \
  tests/architecture/CMakeLists.txt
git commit -m "feat: expose Python topology native API"
```

### Task 7: Implement allowlisted import preflight and the factory manifest

**Files:**
- Create: `bindings/python/hpactor/_topology.py`
- Modify: `bindings/python/hpactor/_errors.py`
- Modify: `bindings/python/hpactor/_runtime.py`
- Modify: `bindings/python/hpactor/__init__.py`
- Create: `bindings/python/tests/unit/test_topology_policy.py`
- Create: `bindings/python/tests/unit/test_topology_manifest.py`
- Create: `bindings/python/tests/fixtures/topology_app/__init__.py`
- Create: `bindings/python/tests/fixtures/topology_app/actors.py`

**Interfaces:**
- Consumes: native descriptor tuples, `Actor`, decorator metadata, frozen `MessageRegistry`, runtime actor installation, and native topology completion.
- Produces: public `PythonTopologyPolicy`, `TopologyPhase`, `TopologyError`; private immutable `_TopologyDescriptor`, `_TopologyFactoryRecord`, `_TopologyFactoryManifest`; and runtime install/rollback handlers.

- [ ] **Step 1: Write failing policy and manifest tests**

```python
class TopologyPolicyTest(unittest.TestCase):
    def test_prefix_matches_exact_or_dotted_child_only(self) -> None:
        policy = PythonTopologyPolicy(("topology_app.actors",))
        self.assertTrue(policy.allows("topology_app.actors"))
        self.assertTrue(policy.allows("topology_app.actors.billing"))
        self.assertFalse(policy.allows("topology_app.actors_evil"))


class TopologyManifestTest(unittest.IsolatedAsyncioTestCase):
    async def test_imports_once_and_freezes_exact_factory_records(self) -> None:
        descriptors = two_echo_descriptors()
        manifest = await _TopologyFactoryManifest.preflight(
            descriptors, PythonTopologyPolicy(("topology_app",)), registry())
        self.assertTrue(manifest.frozen)
        self.assertEqual(topology_app.actors.import_count, 1)
        self.assertNotEqual(manifest.token_for(0), manifest.token_for(1))
        with self.assertRaises(TopologyError):
            manifest.add_for_test(descriptors[0])
```

Add rejection tests for empty policy, lookalike module, relative/path/local
names, import exception, missing attribute, non-class, non-Actor, abstract,
undecorated, different classes reusing one actor type name, positional-only
required constructor, missing/unexpected keyword, and argument fingerprint
mismatch. Assert errors never contain argument values.

- [ ] **Step 2: Run topology unit tests to verify RED**

```bash
PYTHONPATH=bindings/python:bindings/python/tests/fixtures \
python3 -m unittest discover -s bindings/python/tests/unit \
  -p 'test_topology_*.py' -v
```

Expected: imports fail because `_topology.py` and public errors do not exist.

- [ ] **Step 3: Implement immutable policy and structured errors**

```python
@dataclass(frozen=True, slots=True)
class PythonTopologyPolicy:
    allowed_module_prefixes: tuple[str, ...]

    def __post_init__(self) -> None:
        prefixes = tuple(dict.fromkeys(self.allowed_module_prefixes))
        if not prefixes or any(not _is_absolute_module_name(p)
                               for p in prefixes):
            raise ValueError("allowed_module_prefixes must contain absolute module names")
        object.__setattr__(self, "allowed_module_prefixes", prefixes)

    def allows(self, module: str) -> bool:
        return any(module == prefix or module.startswith(prefix + ".")
                   for prefix in self.allowed_module_prefixes)

    @property
    def fingerprint(self) -> int:
        parts = tuple(sorted(prefix.encode("utf-8")
                             for prefix in self.allowed_module_prefixes))
        return _fnv1a64_length_prefixed(parts)


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
```

`TopologyError` extends `HPActorError` and stores phase, optional actor ID, and
optional behavior. Implement `_is_absolute_module_name()` with the same
ASCII-segment rules as native preparation and `_fnv1a64_length_prefixed()` with
offset basis `14695981039346656037` and prime `1099511628211`. Validate policy
prefixes with the approved grammar and reject an empty tuple.

- [ ] **Step 4: Implement preflight exclusively on the actor loop**

Use `importlib.import_module(module)`, then resolve each validated qualname
segment with `getattr`. Require `inspect.isclass`, `issubclass(value, Actor)`,
`not inspect.isabstract(value)`, and valid `__hpactor_actor_name__`. Bind
constructor kwargs with `inspect.signature(value).bind(**dict(args))`.

Group imports by module, but allocate one nonzero monotonically increasing
64-bit token per configured actor. Freeze records in topology order as:

```python
@dataclass(frozen=True, slots=True)
class _TopologyFactoryRecord:
    topology_index: int
    factory_token: int
    actor_class: type[Actor]
    args: MappingProxyType[str, str]
    args_fingerprint: int
```

Assert `asyncio.get_running_loop()` is the dedicated runtime loop. Never retain
records on the application loop or native object.

Call native `record_topology_preflight()` for policy/import/class/signature
success or failure using bounded numeric phase/outcome values. Assert repeated
actors import one module once, nested qualified classes resolve, and an imported
module remains in `sys.modules` after a later preflight or startup rollback.

- [ ] **Step 5: Handle topology install and rollback dispatches**

Add `_ActorRuntime.install_topology_actor(dispatch)` which looks up the frozen
record, constructs `actor_class(**args)`, validates/freezes behavior using the
already frozen message registry, installs the runner, awaits `on_start()`, and
maps constructor, behavior, and start failures to their exact outcomes with
bounded detail. The success call is exactly
`complete_topology_actor(record.factory_token, dispatch.system_generation,
dispatch.actor_generation, TopologyActorOutcome.READY, 0, "")`; failure calls
use the same identifiers and their specific outcome/code/detail.

Add `_ActorRuntime.rollback_topology_actor(dispatch)` which cancels in-progress
start, runs `on_stop()` only if start completed, removes the runner, releases
the factory record, and reports `RolledBack` once. Reuse Phase 1B imperative
installation helpers rather than copying actor binding logic.

- [ ] **Step 6: Run policy, manifest, install, and rollback tests**

```bash
PYTHONPATH=bindings/python:bindings/python/tests/fixtures \
python3 -m unittest discover -s bindings/python/tests/unit \
  -p 'test_topology_*.py' -v
```

Expected: policy boundaries, class validation, constructor binding, token
freeze, runtime ownership, install outcomes, and idempotent rollback pass.

- [ ] **Step 7: Commit Python topology preflight**

```bash
git add bindings/python/hpactor bindings/python/tests/unit \
  bindings/python/tests/fixtures
git commit -m "feat: preflight Python topology factories"
```

### Task 8: Add `ActorSystem.from_topology()` and committed name resolution

**Files:**
- Modify: `bindings/python/hpactor/_system.py`
- Modify: `bindings/python/hpactor/_runtime.py`
- Modify: `bindings/python/hpactor/__init__.py`
- Create: `bindings/python/tests/unit/test_topology_system.py`
- Create: `bindings/python/tests/data/topology_python.toml`
- Create: `bindings/python/tests/data/topology_import_failure.toml`
- Create: `bindings/python/tests/data/topology_start_failure.toml`
- Modify: `bindings/python/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 6–7 native/preflight APIs, Phase 1B system lifecycle, registry freeze, runtime thread, and address values.
- Produces: `ActorSystem.from_topology()`, topology-mode `__aenter__`, `ActorSystem.resolve()`, deterministic cleanup, and unchanged imperative mode.

- [ ] **Step 1: Write failing public startup and rollback tests**

```python
class TopologySystemTest(unittest.IsolatedAsyncioTestCase):
    async def test_from_topology_resolves_only_after_commit(self) -> None:
        native = FakeNativeSystem(topology_descriptors=echo_descriptors())
        system = ActorSystem.from_topology(
            data_path("topology_python.toml"), messages=registry(),
            policy=PythonTopologyPolicy(("topology_app",)), _native=native)
        with self.assertRaises(ActorNotReadyError):
            system.resolve("echo")
        async with system:
            ref = system.resolve("echo")
            self.assertEqual(ref.name, "echo")
        self.assertEqual(native.stop_calls, 1)

    async def test_import_failure_never_starts_native_runtime(self) -> None:
        native = FakeNativeSystem(topology_descriptors=missing_descriptors())
        system = ActorSystem.from_topology(
            data_path("topology_import_failure.toml"), messages=registry(),
            policy=PythonTopologyPolicy(("topology_app",)), _native=native)
        with self.assertRaisesRegex(TopologyError, "import"):
            await system.__aenter__()
        self.assertEqual(native.start_prepared_topology_calls, 0)
        self.assertEqual(native.spawned_actor_count, 0)
```

Add missing policy, binary input, malformed descriptor, native prepare failure,
manifest bind failure, actor start failure, cancellation during enter, repeated
enter, resolve missing name, imperative spawn after success, and idempotent exit
tests.

- [ ] **Step 2: Run public topology tests to verify RED**

```bash
PYTHONPATH=bindings/python:bindings/python/tests/fixtures \
python3 -m unittest bindings.python.tests.unit.test_topology_system -v
```

Expected: `from_topology()` and `resolve()` are absent.

- [ ] **Step 3: Add the side-effect-free topology constructor**

```python
@classmethod
def from_topology(
    cls,
    path: str | os.PathLike[str],
    *,
    messages: MessageRegistry,
    policy: PythonTopologyPolicy,
    config: Mapping[str, object] | None = None,
    _native: _NativeSystemProtocol | None = None,
) -> ActorSystem:

    system = cls(messages=messages, config=config, _native=_native)
    system._mode = _SystemMode.TOPOLOGY
    system._topology_path = os.fspath(Path(path).resolve(strict=True))
    system._topology_policy = policy
    return system

def resolve(self, name: str) -> ActorRef:
    self._require_running()
    raw = self._native.resolve_name(_validate_actor_name(name))
    if raw is None:
        raise KeyError(name)
    family, packed_address, port, actor_type, actor_id, incarnation = raw
    address = ActorAddress(family=family, packed_address=packed_address,
                           port=port, actor_type=actor_type,
                           actor_id=actor_id, incarnation=incarnation)
    return ActorRef(address=address, name=name)
```

Store an absolute topology path, policy, and mode only. Do not parse, import,
freeze messages, construct native state, or start threads in the classmethod.
`resolve()` works only in `Running`, validates the name, calls native
`resolve_name()`, and raises `KeyError` for an absent committed name.

- [ ] **Step 4: Implement exact topology-mode enter ordering**

In `__aenter__()`:

1. freeze the message registry;
2. construct a stopped native system;
3. call native `prepare_topology()` through `asyncio.to_thread()`;
4. require the policy if descriptors are nonempty;
5. start the dedicated Python loop and notifier readers;
6. run `_TopologyFactoryManifest.preflight()` on that loop;
7. bind exact token tuples and policy fingerprint natively;
8. run `native.start_prepared_topology` through `asyncio.to_thread()`;
9. enter `Running` only after native commit returns.

On failure/cancellation, marshal manifest cleanup to the runtime loop, call
native stop/rollback, remove readers, join the runtime thread, mark the system
closed, and re-raise the primary `TopologyError`. Never fall back to an empty
imperative system.

For every native prepare/bind/start failure, read `last_topology_error()` and
construct `TopologyError` with the exact `TopologyPhase`, optional actor ID,
optional behavior, stable code, bounded detail, and rollback bits. Preserve the
native exception as `__cause__`; do not derive phase by parsing error text.

- [ ] **Step 5: Preserve imperative behavior and exports**

Keep the existing constructor/`__aenter__()` branch byte-for-byte equivalent
for non-topology mode. Add `PythonTopologyPolicy`, `TopologyPhase`, and
`TopologyError` to `hpactor.__all__` and public annotations. Keep all Phase 1D
import-quiescence behavior: importing `hpactor` still starts no thread or
native system.

- [ ] **Step 6: Run public API and lifecycle tests to verify GREEN**

```bash
PYTHONPATH=bindings/python:bindings/python/tests/fixtures \
python3 -m unittest bindings.python.tests.unit.test_topology_system -v
PYTHONPATH=bindings/python python3 -m unittest \
  bindings.python.tests.unit.test_actor_system \
  bindings.python.tests.unit.test_public_api -v
```

Expected: topology sequencing/cleanup passes and all imperative lifecycle and
export tests remain green.

- [ ] **Step 7: Commit the public topology API**

```bash
git add bindings/python/hpactor bindings/python/tests
git commit -m "feat: add declarative Python topology API"
```

### Task 9: Prove mixed startup, rollback, and architecture invariants end to end

**Files:**
- Create: `bindings/python/tests/integration/test_topology.py`
- Create: `tests/integration/python/test_python_topology_transaction.cpp`
- Modify: `tests/integration/python/CMakeLists.txt`
- Modify: `bindings/python/tests/CMakeLists.txt`
- Modify: `tests/architecture/CMakeLists.txt`
- Modify: `bindings/python/tests/fixtures/topology_app/actors.py`
- Modify: `bindings/python/tests/data/topology_python.toml`

**Interfaces:**
- Consumes: complete Phase 1E native/Python implementation, real `_hpactor`, test C++ factory, generated protobuf registry, metrics/health snapshots, and fault injection.
- Produces: deterministic evidence for mixed topology order, commit visibility, Python messaging, every rollback phase, no residue, readiness, and architecture containment.

- [ ] **Step 1: Add a real installed-module topology workflow**

```python
class TopologyIntegrationTest(unittest.IsolatedAsyncioTestCase):
    async def test_python_actor_commits_and_handles_ask(self) -> None:
        messages = registry()
        system = ActorSystem.from_topology(
            data_path("topology_python.toml"), messages=messages,
            policy=PythonTopologyPolicy(("topology_app",)))
        async with system:
            echo = system.resolve("echo")
            reply = await system.ask(
                echo, StringValue(value="hello"),
                response_type=StringValue, timeout=5.0)
            self.assertEqual(reply.value, "prod:hello")
            snapshot = system.python_snapshot()
            self.assertEqual(snapshot["topology_committed"], 1)
        self.assertEqual(active_hpactor_threads(), [])
```

Run from a temporary directory outside the checkout with only the built module,
package, fixture application, TOML, and generated protobuf dependency present.

- [ ] **Step 2: Add the native mixed C++/Python transaction integration test**

Register one `CppAuditActor`, provide a real Python bridge provider with an
explicit readiness test port, and load topology ordered Python echo then C++
audit. Gate Python readiness and assert neither name resolves, then release and
assert both resolve together and receive `SystemInit` in model order.

Inject failures at import/preflight handoff, bridge adoption, Python ready,
later C++ factory, atomic name commit, and second `SystemInit`. For every case,
assert baseline directory size/names, zero Python runners/bindings/tokens,
readiness false, liveness true while bounded rollback is progressing, joined
runtime thread after rollback, and reverse cleanup order.

- [ ] **Step 3: Add Python constructor/behavior/start rollback fixtures**

Define `ConstructorFails`, `BehaviorFails`, `StartFails`, and `StartBlocks` in
the fixture module. Tests use asyncio events/control ports, not sleeps. Verify
exact `TopologyPhase.ACTOR_START`, bounded detail, no secret argument value in
error/log snapshot, `on_stop()` only after successful `on_start()`, and timeout
cleanup for `StartBlocks`.

- [ ] **Step 4: Add architecture fitness checks**

Enforce:

- no Python parser (`tomllib`, third-party TOML, or topology file reads) under
  `bindings/python/hpactor`;
- no edit adding Python handling to `parse_file_data()`;
- no `Python.h`/`PyObject` outside `python_capi`;
- no RTTI/exceptions/public `std::function` in Phase 1E native files;
- no Python pointer/class/object field in prepared plans, blueprints, provider,
  journal, spawn lease, queues, or lifecycle stages;
- only the startup worker waits on topology readiness;
- binary topology is explicitly rejected for Python behaviors.

- [ ] **Step 5: Run the focused integration and architecture suite**

```bash
ninja -C build _hpactor test_integration_python_binding test_architecture
ctest --test-dir build \
  -R 'PythonTopology|PythonBindingArchitecture' --output-on-failure
PYTHONPATH=build/bindings/python:bindings/python:bindings/python/tests/fixtures \
python3 -m unittest bindings.python.tests.integration.test_topology -v
```

Expected: mixed commit, ask/reply, every fault rollback, no-residue checks, and
architecture scans pass without sleeps.

- [ ] **Step 6: Run supported sanitizer evidence**

On Linux:

```bash
cmake -S . -B build-tsan -GNinja -DENABLE_TSAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
  -DENABLE_PYTHON_BINDINGS=ON
ninja -C build-tsan test_integration_python_binding
ctest --test-dir build-tsan -R 'PythonTopology' --output-on-failure
```

Expected: no race in provider readiness, atomic name visibility, reverse
rollback, token generation, or runtime shutdown. Record the known macOS ARM
sanitizer limitation without claiming macOS sanitizer evidence.

- [ ] **Step 7: Commit end-to-end topology evidence**

```bash
git add bindings/python/tests tests/integration/python \
  tests/architecture/CMakeLists.txt
git commit -m "test: verify Python topology transactions"
```

### Task 10: Publish manual, wheel smoke, operations, and final Phase 1 evidence

**Files:**
- Create: `docs/manual/python/topology.rst`
- Modify: `docs/manual/python/index.rst`
- Modify: `docs/manual/python/deployment.rst`
- Create: `bindings/python/examples/topology.py`
- Create: `bindings/python/examples/topology.toml`
- Modify: `bindings/python/examples/README.md`
- Modify: `bindings/python/tests/wheel/test_reliability_smoke.py`
- Modify: `.github/workflows/python-wheels.yml`
- Modify: `CLAUDE_MEMORY.md`
- Modify: `docs/superpowers/specs/2026-07-03-python-language-binding-design.md`
- Modify: `docs/superpowers/specs/2026-07-06-python-binding-phase1e-declarative-topology-design.md`

**Interfaces:**
- Consumes: complete Phase 1E implementation, Phase 1D four-wheel matrix, Sphinx manual, examples, snapshots/metrics, and issue #426.
- Produces: executable topology guidance, installed-wheel compatibility evidence, exact operations/security boundaries, and final in-process Phase 1 status.

- [ ] **Step 1: Add failing documentation and wheel-coverage tests**

Extend documentation coverage to require `topology.rst`, its toctree entry,
`PythonTopologyPolicy`, exact `python:module:qualname` syntax, trusted-config
warning, TOML-only/startup-only/local-only boundaries, rollback semantics, and
the executable example. Extend wheel smoke to assert one configured actor can
start, resolve, answer, and shut down outside the checkout.

- [ ] **Step 2: Run documentation/package checks to verify RED**

```bash
python3 -m unittest \
  bindings.python.tests.packaging.test_python_docs -v
sphinx-build -W -b html docs/manual docs/manual/_build/html
```

Expected: the topology page/example and wheel topology case are absent.

- [ ] **Step 3: Write the topology manual and executable example**

Document:

- `behavior = "python:my_app.actors:Echo"` grammar and exact limits;
- constructor keyword values are canonical strings;
- application-side allowlist with exact/dotted-child matching;
- `ActorSystem.from_topology()` and `resolve()` lifecycle;
- mixed C++/Python ordering, external commit, and `SystemInit`;
- import/constructor/start/name failure rollback;
- imports execute trusted code and are not sandboxed or unloaded;
- readiness/metrics/CLI snapshot behavior;
- unsupported binary topology, hot reload, remote placement, paths, URLs,
  entry points, JSON/pickle, and inferred TypeTags.

The example defines an `@actor("echo")` class, explicit protobuf registry,
allowlist, TOML actor, ask/reply, and `async with` shutdown. Execute it against
the installed wheel from a temporary directory.

- [ ] **Step 4: Extend every wheel lane with topology smoke**

Copy only the fixture package and TOML into the clean test directory. On the
same repaired wheel used for Phase 1D compatibility, run topology smoke under
CPython 3.11, 3.12, 3.13, and 3.14 on Linux x86_64/ARM64 and macOS
x86_64/ARM64. Assert import quiescence before system construction and zero
HPActor threads/descriptors after exit.

- [ ] **Step 5: Run full local source, docs, and clean-wheel verification**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
  -DENABLE_PYTHON_BINDINGS=ON
ninja -C build
ctest --test-dir build --output-on-failure --parallel 8
sphinx-build -W -b html docs/manual docs/manual/_build/html
python3 -m unittest discover \
  -s bindings/python/tests -p 'test_*.py' -v
python3 bindings/python/tests/wheel/run_clean_smoke.py \
  --python python3.11 \
  --wheel wheelhouse/hpactor-*-cp311-abi3-*.whl \
  --protobuf 7.35.0
git diff --check
```

Expected: full C++/Python suite, manual, source Python suite, clean installed
wheel topology, and whitespace checks pass.

- [ ] **Step 6: Require four-platform remote acceptance**

Do not mark Phase 1E implemented until all four Phase 1D wheel jobs run the
topology fixture on every supported CPython minor, all binary audits remain
green, no performance metric regresses beyond the Phase 1D 20-percent budget,
and no topology actor/thread/descriptor residue remains.

- [ ] **Step 7: Record exact operations and completion evidence**

Add a dated Phase 1E entry to `CLAUDE_MEMORY.md` with topology syntax, policy
fingerprint, actor counts, rollback fault matrix, test totals, sanitizer result,
wheel matrix, docs build, and remaining binary/hot-reload/remote limitations.

Only after every local and remote gate passes, change statuses to:

```markdown
**Status:** Approved design; Phases 1A through 1E implemented and packaged
```

and:

```markdown
**Status:** Implemented
```

- [ ] **Step 8: Commit Phase 1E documentation and evidence**

```bash
git add docs/manual/python bindings/python/examples \
  bindings/python/tests/wheel/test_reliability_smoke.py \
  .github/workflows/python-wheels.yml CLAUDE_MEMORY.md \
  docs/superpowers/specs/2026-07-03-python-language-binding-design.md \
  docs/superpowers/specs/2026-07-06-python-binding-phase1e-declarative-topology-design.md
git commit -m "docs: complete Python topology phase"
```

## Plan Completion Checklist

- [ ] Phases 1A through 1D pass before Phase 1E implementation begins.
- [ ] The implementation branch contains the current runtime blueprint, coordinator, actor-spawner, and actor-system layout from `main`.
- [ ] Exact `python:<module>:<qualname>` grammar and every byte/count limit are enforced natively.
- [ ] `topology_start_timeout_ms` defaults to 30,000 and accepts only 100–300,000.
- [ ] Native preparation parses once and starts no thread, listener, actor, name, or runtime service.
- [ ] Plain C++ behavior names remain source compatible and Python-prefix collisions fail preflight.
- [ ] Runtime blueprint fingerprints cover ordered actors, behavior, sorted args, and policy fingerprint.
- [ ] The application allowlist is required for Python actors and cannot be widened by TOML.
- [ ] Import uses absolute `importlib.import_module()` without paths, URLs, entry points, evaluation, or `sys.path` mutation.
- [ ] Python modules/classes/signatures are validated before external publication.
- [ ] Factory records live only on the dedicated Python loop and native code sees only nonzero generation-scoped tokens.
- [ ] Actor arguments remain bounded canonical strings and never appear in errors, logs, metrics, CLI, or snapshots.
- [ ] Internally unpublished actor leases fully undo directory, scheduler, lifecycle, mailbox, and name state.
- [ ] Topology names register atomically after every actor is ready.
- [ ] `SystemInit` is delivered only after complete name commit.
- [ ] Only the application startup worker waits for Python topology readiness.
- [ ] Scheduler/network/event-loop/Python threads never wait for topology or call Python incorrectly.
- [ ] Import, constructor, behavior, start, capacity, timeout, later-actor, name, and `SystemInit` failures roll back all actors in reverse order.
- [ ] Primary errors survive secondary rollback failures; rollback bits remain stable and observable.
- [ ] Failed startup leaves no names, directory entries, scheduler registrations, bridges, runners, futures, tokens, callbacks, threads, or descriptors.
- [ ] Imported modules remain in `sys.modules` and binding-owned factory records are released.
- [ ] `ActorSystem.from_topology()` is side-effect free until enter and never falls back to an empty system.
- [ ] Existing imperative Python construction/spawn and plain C++ topology tests remain green.
- [ ] Binary Python topology, hot reload, remote placement, and non-protobuf messages remain explicitly unsupported.
- [ ] Architecture scans prove CPython, RTTI, exception, and Python-object containment.
- [ ] Linux TSAN topology evidence passes or any infrastructure blocker is recorded without a success claim.
- [ ] Manual and example state the trusted executable-config security model and exact unsupported scope.
- [ ] The installed ABI3 wheel topology smoke passes CPython 3.11–3.14 on all four Phase 1D targets.
- [ ] Phase 1E and umbrella design statuses change only after all local and remote gates pass.

## Execution Handoff

Plan complete. Execute only after Phases 1A through 1D acceptance checklists pass and the implementation branch is updated with current `main`. Use `superpowers:subagent-driven-development` for one fresh implementer and two-stage review per task, or `superpowers:executing-plans` for inline batches with review checkpoints.
