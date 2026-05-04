# Actor TOML Configuration — Implementation Plan

## Overview

This plan breaks down the TOML-based declarative topology bootstrapping system into concrete, sequenced implementation steps. Each phase has clear deliverables and acceptance criteria.

**Related documents:**
- [Actor TOML Config Core Concept](actor-toml-config-core-concept.md) — philosophy and usage
- [Actor TOML Config Architecture](actor-toml-config-architecture.md) — detailed specification

---

## Dependency Graph

```
Phase 1: Schema & Internal Model
    │
    ▼
Phase 2: Behavior Factory Registration
    │
    ▼
Phase 3: Runtime TOML Parser
    │
    ├──────────────────────────────┐
    ▼                              ▼
Phase 4: Bootstrap Engine      Phase 5: AOT Compiler (C++)
    │                              │
    └──────────┬───────────────────┘
               ▼
Phase 6: Integration, Tests, Examples
```

---

## Phase 1: Schema and Internal Model

**Goal:** Define the C++ data structures that represent a parsed topology, independent of any parsing mechanism.

### Step 1.1 — Topology model header

File: `include/hpactor/config/topology_model.hpp`

```cpp
namespace hpactor::config {

enum class DispatchPolicy : uint8_t {
    Cooperative = 0,
    DedicatedThread,
    DedicatedPool,
};

struct ResourceSpec {
    uint32_t slab_class_bytes{0};
    uint32_t max_memory_kb{0};
};

struct DispatcherDef {
    std::string name;
    uint16_t threads{1};
    std::vector<uint8_t> cpu_affinity;
};

struct ActorDef {
    std::string id;
    std::string behavior;
    std::string supervisor;
    std::string dispatcher;
    DispatchPolicy dispatch_policy{DispatchPolicy::Cooperative};
    uint32_t mailbox_capacity{0};
    ResourceSpec resources;
    std::unordered_map<std::string, std::string> args;
};

struct SystemDef {
    std::string version;
    uint32_t scheduler_threads{4};
    uint32_t max_queue_depth{1024};
    uint32_t default_mailbox_size{1024};
    bool enable_network{false};
    uint16_t tcp_port{0};
    uint16_t udp_port{5353};
    uint32_t spawn_timeout_ms{5000};
    bool enable_http_gateway{false};
    std::string http_bind_host{"0.0.0.0"};
    uint16_t http_port{8080};
    uint32_t http_max_connections{1000};
    uint32_t http_max_request_size{1048576};
    uint32_t http_reply_timeout_ms{5000};
    bool use_coroutines{false};
    std::vector<std::string> imports;
};

struct TopologyModel {
    SystemDef system;
    std::vector<DispatcherDef> dispatchers;
    std::vector<ActorDef> actors;  // Topologically sorted after Phase 3
};

} // namespace hpactor::config
```

Key type `DispatchPolicy` duplicates the existing `sched::DispatchPolicy` enum intentionally — it's the config-facing version with string parsing support, avoiding coupling the scheduler header to config parsing.

### Step 1.2 — Config directory

```bash
mkdir -p include/hpactor/config
```

### Acceptance

- `TopologyModel` compiles cleanly with no dependencies beyond standard library
- All fields documented with default values

---

## Phase 2: Behavior Factory Registration

**Goal:** Build the static registration mechanism that maps behavior name strings to factory functions, enabling `ActorSystem::spawn<T>()` dispatch from TOML-derived data.

### Step 2.1 — Factory function type alias

File: `include/hpactor/config/actor_factory.hpp`

```cpp
namespace hpactor::config {

using ActorFactory = std::function<std::shared_ptr<AbstractActor>(
    ActorContext*, ActorSystem&)>;

} // namespace hpactor::config
```

### Step 2.2 — Factory registry

File: `include/hpactor/config/actor_factory_registry.hpp`

```cpp
namespace hpactor::config {

class ActorFactoryRegistry {
  public:
    static ActorFactoryRegistry& instance();

    template <typename T>
    void register_factory(const std::string& name);

    ActorFactory get_factory(const std::string& name) const;
    bool has(const std::string& name) const;
    std::vector<std::string> known_names() const;

  private:
    std::unordered_map<std::string, ActorFactory> factories_;
};

// Template implementation
template <typename T>
void ActorFactoryRegistry::register_factory(const std::string& name) {
    factories_.emplace(name, [](ActorContext* ctx, ActorSystem& sys) {
        auto actor = std::make_shared<T>(ctx, sys);
        return actor;
    });
}

} // namespace hpactor::config
```

**Design decision:** A separate global singleton instead of extending `ActorTypeRegistry`. The existing `ActorTypeRegistry` is wired for remote spawn protocol (protobuf message types, wire serialization). The config factory is a simpler, local-only concern. They can be unified later if needed.

### Step 2.3 — Registration macro

File: `include/hpactor/config/actor_factory_registry.hpp` (append)

```cpp
#define HPACTOR_REGISTER_ACTOR(Name, ActorClass)                             \
    namespace {                                                               \
        [[maybe_unused]] static const bool _hpactor_reg_##ActorClass = [] {   \
            hpactor::config::ActorFactoryRegistry::instance()                \
                .register_factory<ActorClass>(Name);                         \
            return true;                                                      \
        }();                                                                  \
    }
```

### Step 2.4 — Unit test

File: `tests/test_actor_factory_registry.cpp`

Test cases:
1. Register two types, verify `has()` returns true for both
2. Call `get_factory("name")` and construct an actor, verify type
3. `has()` returns false for unknown name
4. `known_names()` returns all registered names
5. Duplicate registration overwrites (last wins)

### Acceptance

- `ActorFactoryRegistry` compiles and passes all unit tests
- Macro can be placed in any `.cpp` file and registers before `main()`

---

## Phase 3: Runtime TOML Parser

**Goal:** Parse TOML files into `TopologyModel`, implementing import resolution, template inheritance, validation, and topological sort.

### Step 3.1 — Add toml++ dependency

`toml++` is a C++20 header-only TOML parser. Add as a vendored dependency.

File: `cmake/tomlplusplus.cmake`

```cmake
include(FetchContent)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)
```

Update `CMakeLists.txt` to include this for the `hpactor_lib` target.

### Step 3.2 — Import resolution

File: `src/config/toml_parser.cpp`

```cpp
namespace hpactor::config {

// Resolve imports relative to entrypoint directory.
// Supports glob patterns (via std::filesystem or minimal glob impl).
// Returns list of files to merge, in DFS order.
std::vector<std::string> resolve_imports(
    const std::string& entrypoint_path,
    const std::vector<std::string>& imports);

} // namespace hpactor::config
```

**Glob implementation:** Use `<filesystem>` for directory iteration with simple pattern matching (`*` only). No external dependency needed.

**Design rule:** Imported files must NOT contain `[system]` or `imports` sections. If detected, fail with a clear error citing the file path.

### Step 3.3 — TOML merge

File: `src/config/toml_parser.cpp` (append)

```
merge_files(entrypoint_path):
    1. Parse entrypoint TOML → system section, root actors/dispatchers/templates
    2. For each import in system.imports:
       a. Expand globs relative to entrypoint directory
       b. For each resolved file:
          - Parse TOML
          - Append [[actor]] entries to merged list
          - Append [[dispatcher]] entries to merged list
          - Merge [template.*] entries (first-wins: skip duplicates)
    3. Append entrypoint's own [[actor]] and [[dispatcher]] to merged lists
    4. Return merged (system_config, actors, dispatchers, templates)
```

### Step 3.4 — Template resolution

File: `src/config/toml_parser.cpp` (append)

```
resolve_templates(actors, templates):
    for each actor in actors:
        if actor.inherits is set:
            if actor.inherits not in templates: FAIL
            base = deep_copy(templates[actor.inherits])
            deep_merge(base, actor)  // actor values override
            replace actor with merged result
```

**Deep merge rules:**
- Scalar fields: if actor value is non-default, use actor value; else use template value
- `args` map: merge key-by-key; actor keys override template keys
- `resources`: merge key-by-key; actor values override template values
- `inherits` is consumed during resolution (not carried into `ActorDef`)

### Step 3.5 — Validation

File: `src/config/toml_parser.cpp` (append)

```
validate(model):
    // Identity
    ids = set()
    for actor in model.actors:
        if actor.id in ids: FAIL "duplicate actor id '{actor.id}'"
        ids.add(actor.id)
        if actor.behavior.empty(): FAIL "actor '{actor.id}' has no behavior"

    // Dispatcher references
    disp_names = set(d.name for d in model.dispatchers)
    for actor in model.actors:
        if not actor.dispatcher.empty() and actor.dispatcher not in disp_names:
            FAIL "actor '{actor.id}' references unknown dispatcher '{actor.dispatcher}'"

    // Supervisor references (post-sort check: every supervisor must be a valid actor id)
```

### Step 3.6 — Topological sort

File: `src/config/toml_parser.cpp` (append)

```
topological_sort(actors):
    // Build adjacency: supervisor → children
    id_to_actor = map(id → actor)
    children = map(supervisor_id → vector<actor_id>)

    // Detect missing supervisor references
    for actor in actors:
        if not actor.supervisor.empty() and actor.supervisor not in id_to_actor:
            FAIL "actor '{actor.id}' references unknown supervisor '{actor.supervisor}'"

    // Kahn's algorithm
    in_degree = map(actor_id → 0)
    for actor in actors:
        if not actor.supervisor.empty():
            in_degree[actor.id] += 1

    queue = [actor.id for actor in actors if actor.supervisor.empty()]
    sorted = []

    while queue not empty:
        current = queue.pop_front()
        sorted.append(current)
        for child in children[current]:
            in_degree[child] -= 1
            if in_degree[child] == 0:
                queue.push_back(child)

    if sorted.size() != actors.size():
        FAIL "circular supervisor dependency detected"

    // Reorder actors array by sorted order
    return sorted_actors
```

### Step 3.7 — Public API

File: `include/hpactor/config/toml_parser.hpp`

```cpp
namespace hpactor::config {

struct ParseError {
    std::string message;
    std::string file;
    size_t line{0};
};

class TomlParser {
  public:
    // Parse a TOML entrypoint file into a validated, topologically sorted TopologyModel.
    // Returns error on parse failure, validation failure, or circular dependency.
    static result<TopologyModel, ParseError> parse(const std::string& entrypoint_path);
};

} // namespace hpactor::config
```

### Step 3.8 — Unit tests

File: `tests/test_toml_parser.cpp`

Test cases:
1. Parse minimal valid TOML (one actor, no supervisor)
2. Parse multi-actor with supervisor hierarchy
3. Template inheritance: scalar override
4. Template inheritance: args merge
5. Import resolution: actors from two files merged
6. Topological sort: linear chain (A→B→C)
7. Topological sort: diamond (A→B, A→C, B→D, C→D)
8. Duplicate actor id → error
9. Unknown supervisor reference → error
10. Circular dependency → error
11. Unknown dispatcher reference → error
12. Missing template reference → error
13. Glob import pattern matches multiple files

**Test data:** Create inline TOML strings or small fixture files in `tests/data/toml/`.

### Acceptance

- All 13 parser test cases pass
- Valid TOML produces correct `TopologyModel` with sorted actors
- Invalid TOML produces clear `ParseError` messages

---

## Phase 4: Bootstrap Engine

**Goal:** Consume a `TopologyModel` and spawn all actors into a live `ActorSystem`.

### Step 4.1 — Engine implementation

File: `src/config/bootstrap_engine.cpp`

```cpp
namespace hpactor::config {

class BootstrapEngine {
  public:
    explicit BootstrapEngine(TopologyModel model);

    // Execute the full spawn sequence. Returns error on first failure.
    result<void, BootstrapError> execute(ActorSystem& system);

  private:
    // Phase 2: Create dispatcher thread pools
    result<void, BootstrapError> create_dispatchers(ActorSystem& system);

    // Phase 3: Verify all behaviors are registered in ActorFactoryRegistry
    result<void, BootstrapError> validate_behaviors();

    // Phase 5: Spawn actors in topological order
    result<void, BootstrapError> spawn_actors(ActorSystem& system);

    // Phase 6: Broadcast SystemInit to all spawned actors
    void broadcast_system_init(ActorSystem& system);

    TopologyModel model_;
    std::vector<ActorId> spawned_ids_;
};

} // namespace hpactor::config
```

### Step 4.2 — Dispatcher creation

```
create_dispatchers(system):
    for each dispatcher_def in model_.dispatchers:
        pool_config = SchedulerPoolConfig{
            .name = dispatcher_def.name,
            .thread_count = dispatcher_def.threads,
            .cpu_affinity = dispatcher_def.cpu_affinity,
        }
        system.scheduler()->create_pool(pool_config)
```

**Integration note:** The current scheduler (`sched::IScheduler`) may not yet have a `create_pool` method. If absent, implement as part of this step:
- Add `virtual void create_pool(const SchedulerPoolConfig&) = 0` to `IScheduler`
- Implement in `HybridScheduler` to create a named work-stealing pool

If scheduler changes are too invasive for this phase, defer: pool creation can be a no-op that logs a warning, with full pool-per-dispatcher support tracked as a follow-up.

### Step 4.3 — Actor spawning

```
spawn_actors(system):
    spawned_actors = map<id, Actor>{}

    for actor_def in model_.actors:
        // 1. Look up factory
        factory = ActorFactoryRegistry::instance().get_factory(actor_def.behavior)
        if not factory: return Error("unknown behavior: " + actor_def.behavior)

        // 2. Construct actor via factory (default ctor)
        actor_ptr = factory(nullptr, system)

        // 3. Spawn via ActorSystem (assigns id, creates mailbox, context, activates)
        actor = system.spawn_configured(actor_ptr, actor_def)

        // 4. Configure from args if actor supports it
        if actor_ptr has configure_from_args:
            result = actor_ptr->configure_from_args(actor_def.args)
            if not result: return Error("config failed for " + actor_def.id)

        // 5. Register in name registry
        system.registry().put(actor_def.id, actor_ptr->address())

        // 6. Track for SystemInit broadcast
        spawned_ids_.push_back(actor_ptr->id())
        spawned_actors[actor_def.id] = actor

    return spawned_ids_
```

**New `ActorSystem` method required:** `spawn_configured(shared_ptr<AbstractActor>, ActorDef)` — variant of `spawn<T>()` that accepts a pre-constructed actor and applies `ActorDef` fields (mailbox capacity, dispatch policy, dispatcher pool) during spawn setup.

### Step 4.4 — ActorSystem::spawn_configured()

File: `include/hpactor/core/actor_system.hpp` (append to ActorSystem class)

```cpp
// Spawn a pre-constructed actor with configuration from ActorDef.
// Used by BootstrapEngine. The actor is already constructed (via factory);
// this method handles ActorId assignment, mailbox creation, context setup,
// and scheduler registration.
Actor spawn_configured(std::shared_ptr<AbstractActor> actor,
                       const config::ActorDef& def);
```

File: `src/actor/actor_system.cpp` (implement)

Implementation is a refactored version of `spawn<T>()` that takes an already-constructed `shared_ptr<AbstractActor>` instead of constructing via `make_shared<T>`. The dispatch policy, dispatcher pool, and mailbox capacity are read from `ActorDef` rather than from the actor's virtual methods.

### Step 4.5 — configure_from_args interface

File: `include/hpactor/config/actor_args.hpp`

```cpp
namespace hpactor::config {

// Actors implement this to receive TOML args during bootstrap.
// The bootstrap engine calls configure_from_args() on each actor
// after construction, if the actor type supports it.
template <typename T>
concept ConfigurableActor = requires(T& actor, 
    const std::unordered_map<std::string, std::string>& args) {
    { T::configure_from_args(args, actor) } -> std::same_as<result<void>>;
};

} // namespace hpactor::config
```

Detection in `spawn_configured()`:
```cpp
if constexpr (ConfigurableActor<std::remove_reference_t<decltype(*actor)>>) {
    auto config_result = T::configure_from_args(def.args, *actor);
    if (!config_result) return config_result.error();
}
```

### Step 4.6 — SystemInit message

File: `include/hpactor/types/types.hpp` (append)

```cpp
constexpr TypeTag SystemInitTag = TypeTag(6);  // Reserved system range 1-99
```

The message has no payload — the tag alone is the signal. The existing `TypedMessage` infrastructure carries it.

File: `src/config/bootstrap_engine.cpp` (in `broadcast_system_init()`)

```cpp
for (ActorId id : spawned_ids_) {
    TypedMessage msg(SystemInitTag);
    msg.set_sender_address(system.system_actor()->address());
    system.deliver_local(id, std::move(msg));
}
```

### Step 4.7 — ActorSystem::load_topology() convenience

File: `src/actor/actor_system.cpp` (append)

```cpp
result<void, config::BootstrapError> ActorSystem::load_topology(
    const std::string& toml_path) 
{
    auto parse_result = config::TomlParser::parse(toml_path);
    if (!parse_result) return parse_result.error();

    config::BootstrapEngine engine(std::move(*parse_result));
    return engine.execute(*this);
}
```

### Step 4.8 — Unit and integration tests

File: `tests/test_bootstrap_engine.cpp`

Test cases:
1. Single actor: spawn from TOML, verify actor is registered and reachable
2. Parent-child: supervisor receives SystemInit before child
3. Dispatcher assignment: actor gets correct pool
4. Args: actor receives and applies configuration via `configure_from_args()`
5. SystemInit delivery: actor receives SystemInit message
6. Multiple roots: two independent trees spawn correctly
7. Unknown behavior → BootstrapError (fail-fast)

File: `tests/test_system_init.cpp`

Test case:
1. Actor gates traffic until SystemInit received (stashes messages, processes after)

### Acceptance

- `BootstrapEngine::execute()` spawns the full tree from a valid `TopologyModel`
- `ActorSystem::load_topology("main.toml")` is a working end-to-end path
- All 7 bootstrap engine tests pass
- SystemInit test passes

---

## Phase 5: AOT Compiler (C++)

**Goal:** A build-time C++ tool that compiles TOML topology files into a FlatBuffers binary for zero-copy startup. Sharing the same TOML parsing logic as the runtime path (Phase 3), the AOT compiler is a standalone executable built as part of the CMake project.

### Step 5.1 — FlatBuffers schema

File: `tools/toml-compiler/topology.fbs`

```flatbuffers
namespace hpactor.config;

struct ResourceSpec {
    slab_class_bytes: uint32;
    max_memory_kb: uint32;
}

table KeyValue {
    key: string (key);
    value: string;
}

table Dispatcher {
    name: string (key);
    threads: uint16;
    cpu_affinity: [uint8];
}

table Actor {
    id: string (key);
    behavior: string;
    supervisor_id: string;
    dispatcher_name: string;
    dispatch_policy: uint8;
    mailbox_capacity: uint32;
    resources: ResourceSpec;
    args: [KeyValue];
}

table SystemConfig {
    version: string;
    scheduler_threads: uint32;
    max_queue_depth: uint32;
    default_mailbox_size: uint32;
    enable_network: bool;
    tcp_port: uint16;
    udp_port: uint16;
    spawn_timeout_ms: uint32;
    enable_http_gateway: bool;
    http_port: uint16;
    http_bind_host: string;
    http_max_connections: uint32;
    http_max_request_size: uint32;
    http_reply_timeout_ms: uint32;
    use_coroutines: bool;
}

table SystemTopology {
    system: SystemConfig;
    dispatchers: [Dispatcher];
    actors: [Actor];
}

root_type SystemTopology;
```

### Step 5.2 — AOT Compiler executable

File: `tools/toml-compiler/compiler.cpp`

Dependencies: `toml++` (same header-only library as Phase 3), `flatc`-generated C++ headers, libflatbuffers.

The compiler links against the same TOML parsing logic built in Phase 3 (`src/config/toml_parser.cpp`), sharing import resolution, template resolution, validation, and topological sort. The only difference is the final serialization step — FlatBuffers instead of `TopologyModel`.

```text
compiler.cpp phases:
    1. Parse CLI args: --input main.toml --output topology.bin
    2. Load and merge TOML (reuses TomlParser merge/validate/sort logic)
    3. FlatBuffers serialization via FlatBufferBuilder (bottom-up construction)
    4. Write topology.bin to output path
```

**File structure:**
```
tools/toml-compiler/
├── CMakeLists.txt             # Builds the hpactor_toml_compiler executable
├── compiler.cpp               # CLI entry point
├── flatbuffers_serializer.hpp  # TopologyModel → FlatBuffers binary
└── flatbuffers_serializer.cpp  # Serialization implementation
```

**CLI:**
```bash
# Built as part of the CMake project:
cmake -S . -B build -GNinja
ninja -C build hpactor_toml_compiler

# Run at build time or manually:
./build/tools/toml-compiler/hpactor_toml_compiler \
    --input config/main.toml \
    --output build/topology.bin
```

### Step 5.3 — C++ FlatBuffers loader

File: `src/config/flatbuffers_loader.cpp`

```cpp
namespace hpactor::config {

result<TopologyModel, LoadError> load_from_binary(const std::string& path) {
    // 1. open + fstat
    // 2. mmap with MAP_PRIVATE
    // 3. GetSystemTopology(data)
    // 4. Walk FlatBuffers tables → populate TopologyModel
    // 5. munmap
    // 6. return model
}

} // namespace hpactor::config
```

### Step 5.4 — CMake integration

File: `cmake/toml_compiler.cmake`

The AOT compiler is built as a CMake executable target. A custom command runs it at build time to produce `topology.bin`. The compiler reuses existing library code (`TomlParser`, `TopologyModel`) from the `hpactor_lib` target.

```cmake
# Build the AOT compiler executable (links hpactor_lib for shared parsing logic)
add_executable(hpactor_toml_compiler
    tools/toml-compiler/compiler.cpp
    tools/toml-compiler/flatbuffers_serializer.cpp
)
target_link_libraries(hpactor_toml_compiler PRIVATE hpactor_lib flatbuffers::flatbuffers)

# Custom target: compile TOML → FlatBuffers binary at build time
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/topology.bin
    COMMAND hpactor_toml_compiler
        --input ${TOML_ENTRYPOINT}
        --output ${CMAKE_BINARY_DIR}/topology.bin
    DEPENDS hpactor_toml_compiler ${TOML_SOURCE_FILES}
    COMMENT "Compiling TOML topology → FlatBuffers binary"
)

add_custom_target(compile_topology
    DEPENDS ${CMAKE_BINARY_DIR}/topology.bin
)
```

### Step 5.5 — Round-trip test

File: `tests/test_toml_flatbuffers_roundtrip.cpp`

Test case:
1. Parse TOML → TopologyModel via runtime parser
2. Serialize TopologyModel → FlatBuffers binary (in memory)
3. Load FlatBuffers binary → TopologyModel via loader
4. Assert both models are identical (all fields, all actors)

### Acceptance

- Compiler produces valid `topology.bin` from a TOML entrypoint
- `load_from_binary()` round-trips identically with `TomlParser::parse()`
- `topology.bin` loads without heap allocation (verified via `mmap` + FlatBuffers access)

---

## Phase 6: Integration and Documentation

### Step 6.1 — Example TOML configs

File: `examples/config/`

```
examples/config/
├── minimal.toml              # Single actor
├── supervisor_tree.toml      # 3-level hierarchy
├── worker_pool.toml          # Router + template + 4 workers
├── multi_domain/
│   ├── main.toml
│   ├── dispatchers.toml
│   ├── templates.toml
│   └── domains/
│       ├── auth.toml
│       └── api.toml
└── README.md                 # How to use the TOML config system
```

### Step 6.2 — Update main example

File: `examples/` (create or update an existing example to use TOML loading)

```cpp
// examples/toml_bootstrap/main.cpp
#include <hpactor/core/actor_system.hpp>

int main() {
    hpactor::Config config;
    hpactor::ActorSystem system(config);

    auto result = system.load_topology("examples/config/minimal.toml");
    if (!result) {
        std::cerr << "Bootstrap failed: " << result.error().message << "\n";
        return 1;
    }

    std::cout << "All actors spawned. Running...\n";
    // System runs until shutdown signal
}
```

### Step 6.3 — Register existing actor types

Add `HPACTOR_REGISTER_ACTOR` macros to any actors that should be bootable from TOML configs. This is opt-in: only actors explicitly registered can be referenced in config files.

### Step 6.4 — Update CLAUDE.md and project docs

- Add config build commands to CLAUDE.md
- Add new files to project overview
- Update project memory with new phase

### Acceptance

- All examples compile and run without error
- CI builds the AOT compiler and validates `topology.bin` output
- `ctest` includes all new test suites in the test run

---

## Summary Table

| Phase | New Files | Modified Files | Tests | Duration Estimate |
|-------|-----------|---------------|-------|-------------------|
| 1. Schema | `topology_model.hpp` | — | Compile check | Small |
| 2. Factory | `actor_factory.hpp`, `actor_factory_registry.hpp` | — | `test_actor_factory_registry.cpp` (5) | Small |
| 3. Parser | `toml_parser.hpp`, `toml_parser.cpp` | `CMakeLists.txt` | `test_toml_parser.cpp` (13) | Large |
| 4. Engine | `bootstrap_engine.cpp`, `actor_args.hpp` | `actor_system.hpp`, `actor_system.cpp` | `test_bootstrap_engine.cpp` (7), `test_system_init.cpp` (1) | Large |
| 5. AOT | `topology.fbs`, `compiler.cpp`, `flatbuffers_serializer.cpp/.hpp`, `flatbuffers_loader.cpp` | `cmake/toml_compiler.cmake`, `tools/toml-compiler/CMakeLists.txt` | `test_toml_flatbuffers_roundtrip.cpp` (1) | Medium |
| 6. Integration | Example configs, updated example main | `CLAUDE.md`, memory | — | Small |

**Total estimated new files:** ~12 source/header, 4 test files, 5 example configs
**Total estimated test cases:** ~27

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Scheduler has no `create_pool()` API | Blocks dispatcher-per-pool feature | Default to single pool; add pool API as follow-up |
| `toml++` API changes between versions | Build break | Pin exact version tag in FetchContent |
| `ActorSystem::spawn<T>()` template is not easily refactorable to `spawn_configured()` | Duplication or rework | Extract shared spawn logic into private `spawn_impl()` helper |
| `configure_from_args()` concept detection fails on some compilers | Build break | Guard with `#ifdef` / feature macro; fall back to runtime detection |
| AOT compiler links against `hpactor_lib` — circular dependency risk | `hpactor_lib` doesn't depend on AOT compiler; compiler is a separate executable target | Ensure `hpactor_toml_compiler` only links, not co-compiled |
| FlatBuffers schema drift from TopologyModel | Roundtrip test fails | Phase 5.5 roundtrip test catches schema/model mismatches immediately |

---

## Order of Execution

Phases should be implemented in order: 1 → 2 → 3 → 4 → 5 → 6.

Phase 5 (AOT) can be started in parallel with Phase 4 (Engine) since they share only the `TopologyModel` interface, which is stable after Phase 1.

Each phase should be a separate PR/commit, with tests passing before moving to the next.
