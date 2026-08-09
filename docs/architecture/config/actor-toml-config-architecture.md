# Actor TOML Configuration — Architecture Design

## Overview

This document specifies the architecture for loading actor topologies from TOML configuration files. It covers the bootstrap engine, schema design, module system, template inheritance, AOT compilation path, and integration with the existing HPActor spawn infrastructure.

**Related documents:**
- [Actor Core Concept](../actor/actor-core-concept.md) — actor type hierarchy and messaging model
- [Actor TOML Config Core Concept](actor-toml-config-core-concept.md) — philosophy and usage overview
- [TOML Config Implementation Plan](../../superpowers/plans/2026-05-03-toml-config-topology-impl.md) — phased implementation steps

---

## Architecture

### System Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                        BUILD TIME (AOT Path)                         │
│                                                                     │
│  main.toml  ─┐                                                      │
│  pools.toml ─┤                                                      │
│  workers/   ─┼──►  AOT Compiler (C++ tool)                           │
│  *.toml     ─┘         │                                            │
│                        ├─ Parse + merge all TOML files              │
│                        ├─ Resolve template inheritance              │
│                        ├─ Validate id uniqueness                    │
│                        ├─ Topological sort (DAG)                    │
│                        └─ Serialize to HPActor binary topology       │
│                                        │                            │
│                              topology.bin                           │
└─────────────────────────────────────────────────────────────────────┘
                                       │
                                       ▼
┌─────────────────────────────────────────────────────────────────────┐
│                       RUNTIME (Both Paths)                           │
│                                                                     │
│  topology.bin ─────┐                                                │
│                    │    ┌──────────────────────────┐                 │
│  main.toml  ──────┼──►│     Bootstrap Engine      │                 │
│  (runtime path)    │   │                          │                 │
│                    │   │  1. Load topology model  │                 │
│                    │   │  2. Validate behaviors   │                 │
│                    │   │     against ActorType    │                 │
│                    │   │     Registry             │                 │
│                    │   │  3. Create dispatchers   │                 │
│                    │   │  4. Batch pre-allocate   │                 │
│                    │   │     mailboxes + contexts  │                 │
│                    │   │  5. Spawn in DAG order   │                 │
│                    │   │  6. Broadcast SystemInit │                 │
│                    └──────────┬───────────────────┘                 │
│                               │                                     │
│                               ▼                                     │
│                    ┌──────────────────────┐                         │
│                    │   ActorSystem          │                        │
│                    │   .spawn<T>(...)       │  ← Existing API        │
│                    │   .registry()          │                        │
│                    │   .deliver_local()     │                        │
│                    └──────────────────────┘                         │
└─────────────────────────────────────────────────────────────────────┘
```

### Two Loading Paths

```
                    ┌─────────────┐
                    │  Bootstrap  │
                    │   Engine    │
                    └──────┬──────┘
                           │
              ┌────────────┴────────────┐
              ▼                         ▼
    ┌──────────────────┐     ┌──────────────────┐
    │  RUNTIME PATH     │     │   AOT PATH        │
    │                   │     │                   │
    │  toml++ parser    │     │  mmap()           │
    │       │           │     │       │           │
    │       ▼           │     │       ▼           │
    │  In-memory AST    │     │  BinaryLoader     │
    │  (std::vector,    │     │  (custom format)  │
    │   std::string)    │     │  string-table read│
    │       │           │     │       │           │
    └───────┼───────────┘     └───────┼───────────┘
            │                         │
            └─────────┬───────────────┘
                      ▼
         ┌──────────────────────┐
         │  TopologyModel       │
         │  (internal repr)     │
         │                      │
         │  - dispatchers[]     │
         │  - actors[] (sorted) │
         │  - system_config     │
         └──────────┬───────────┘
                    │
                    ▼
         ┌──────────────────────┐
         │  Spawn Execution     │
         └──────────────────────┘
```

---

## TOML Schema Specification

### `[system]` — Global Configuration

Maps to `Config` struct fields.

```toml
[system]
version = "1.0"                  # Schema version (required)
scheduler_threads = 4            # → Config::scheduler_threads
max_queue_depth = 1024           # → Config::max_queue_depth
default_mailbox_size = 1024     # Default mailbox capacity for all actors

# Network (optional — requires enable_network)
enable_network = false
tcp_port = 0
udp_port = 5353
spawn_timeout_ms = 5000

# HTTP (optional — requires enable_network)
enable_http_gateway = false
http_port = 8080
http_bind_host = "0.0.0.0"
http_max_connections = 1000
http_max_request_size = 1048576

# Coroutines
use_coroutines = false

# Module imports (preprocessor directive)
imports = [
    "dispatchers.toml",
    "domains/*.toml"             # Glob patterns supported
]
```

### `[[dispatcher]]` — Scheduler Pool Configuration

Maps to scheduler thread pool creation.

```toml
[[dispatcher]]
name = "io_pool"                 # Unique name (referenced by actors)
threads = 4                      # Number of worker threads
cpu_affinity = [0, 1, 2, 3]     # Core pinning (optional, empty = no pinning)
```

**Mapping to code:**
- `dispatcher.name` → key in dispatcher registry
- `dispatcher.threads` → scheduler worker thread count for that pool
- `dispatcher.cpu_affinity` → `DispatchHints::cpu_affinity` for each actor assigned to this pool

Multiple `[[dispatcher]]` entries create separate thread pools. Actors reference them by name via the `dispatcher` field.

### `[template]` — Reusable Actor Defaults

```toml
[template.<name>]
behavior = "<BehaviorName>"       # Required
dispatcher = "<pool_name>"       # Optional (default: system default pool)
dispatch_policy = "Cooperative"  # Optional (default: Cooperative)
mailbox_capacity = 1024           # Optional (default: system default_mailbox_size)

  [template.<name>.resources]
  slab_class_bytes = 256
  max_memory_kb = 1024

  [template.<name>.args]
  key1 = "value1"
  key2 = "42"
```

Templates are resolved at merge time. An actor that `inherits` a template gets the template's values as defaults, overridden by any explicitly set fields.

### `[[actor]]` — Actor Instance Definition

```toml
[[actor]]
id = "unique_actor_id"           # Required, globally unique
behavior = "MyActor"             # Required, must be in ActorTypeRegistry
supervisor = "parent_id"         # Optional — spawns as child of this actor
dispatcher = "io_pool"           # Optional — which thread pool
dispatch_policy = "Cooperative"  # Optional — Cooperative | DedicatedThread | DedicatedPool
mailbox_capacity = 4096           # Optional — overrides template/system default
inherits = "base_worker"         # Optional — inherits from [template.base_worker]

  [actor.resources]
  slab_class_bytes = 256          # Optional — pre-allocated slab block size
  max_memory_kb = 1024            # Optional — memory limit

  [actor.args]                    # Optional — key-value constructor arguments
  listen_port = "8080"
  db_connection_string = "host=localhost"
  max_connections = "1000"
```

**Field validation rules:**

| Field | Required | Unique | Default |
|-------|----------|--------|---------|
| `id` | Yes | Globally | — |
| `behavior` | Yes (or via template) | No | — |
| `supervisor` | No | No | `null` (system-level spawn) |
| `dispatcher` | No | No | System default pool |
| `dispatch_policy` | No | No | `"Cooperative"` |
| `mailbox_capacity` | No | No | `system.default_mailbox_size` |
| `inherits` | No | No | `null` |
| `resources` | No | No | Unbounded |
| `args` | No | No | Empty dict |

### Enums and Value Mapping

```
dispatch_policy values:
  "Cooperative"       → DispatchPolicy::Cooperative
  "DedicatedThread"   → DispatchPolicy::DedicatedThread
  "DedicatedPool"     → DispatchPolicy::DedicatedPool
```

---

## Bootstrap Engine

### Component: `BootstrapEngine`

```cpp
namespace hpactor::config {

class BootstrapEngine {
  public:
    // Load from TOML file (runtime path)
    static TopologyModel load_from_toml(const std::string& path);

    // Load from pre-compiled HPActor binary topology (AOT path)
    static TopologyModel load_from_binary(const std::string& path);

    // Execute spawn of entire topology into an ActorSystem
    void execute(ActorSystem& system);
};

} // namespace hpactor::config
```

### Internal Representation: `TopologyModel`

```cpp
struct DispatcherDef {
    std::string name;
    uint16_t threads;
    std::vector<uint8_t> cpu_affinity;
};

struct ActorDef {
    std::string id;
    std::string behavior;
    std::string supervisor;       // empty = system-level
    std::string dispatcher;        // empty = default pool
    DispatchPolicy dispatch_policy{DispatchPolicy::Cooperative};
    uint32_t mailbox_capacity{1024};
    uint32_t slab_class_bytes{0};
    uint32_t max_memory_kb{0};
    std::unordered_map<std::string, std::string> args;
};

struct TopologyModel {
    std::string version;
    uint32_t default_mailbox_size;
    // ... other Config fields ...

    std::vector<DispatcherDef> dispatchers;
    std::vector<ActorDef> actors;  // Topologically sorted
};
```

### Execution Flow

```
BootstrapEngine::execute(system)
    │
    ├─ Phase 1: Configure system
    │     system.config_ ← topology system fields
    │
    ├─ Phase 2: Create dispatchers
    │     for each DispatcherDef:
    │       scheduler->create_pool(name, threads, cpu_affinity)
    │
    ├─ Phase 3: Validate behaviors
    │     for each ActorDef:
    │       assert system.actor_type_registry().has(behavior)
    │       (fail-fast with clear error if unknown)
    │
    ├─ Phase 4: Pre-allocate mailboxes
    │     for each ActorDef:
    │       pre-allocate MPSCActorMailbox with specified capacity
    │       pre-allocate ActorContext
    │       (batch allocation reduces allocator churn)
    │
    ├─ Phase 5: Spawn in topological order
    │     for each ActorDef (already sorted):
    │       if supervisor is empty:
    │         system.spawn<T>(args...)
    │       else:
    │         parent = system.resolve_actor(supervisor)
    │         parent->context()->spawn<T>(args...)
    │       register actor.id → actor.address in system.registry()
    │
    └─ Phase 6: Broadcast SystemInit
           for each spawned actor:
             deliver_local(actor.id, SystemInitMessage{})
           (actors begin processing only after this signal)
```

### Topological Sort (DAG)

Actor instantiation respects hierarchy: a supervisor must exist before its children.

```
Algorithm:
  1. Build adjacency: supervisor → [child1, child2, ...]
  2. Detect cycles → fail with error listing the cycle
  3. Kahn's algorithm BFS:
       queue = all actors with no supervisor (roots)
       while queue not empty:
         pop actor, append to sorted list
         for each child of actor:
           remove edge
           if child has no remaining dependencies:
             push child
  4. If sorted count ≠ total actors → cycle detected (fail)

Example:
  Input:
    A (no supervisor)
    B (supervisor = A)
    C (supervisor = A)
    D (supervisor = B)
  Sorted output: [A, B, C, D]
```

System-level actors (no `supervisor`) are roots. All children are spawned via `ActorContext::spawn()`, which automatically establishes the supervision relationship and monitoring.

---

## Module System (Include/Import)

### Problem

The official TOML specification does not support `import` or `include` directives. For large systems with hundreds of actors, a single monolithic file is unmanageable.

### Solution: Preprocessor Merge

Imports are resolved by the loader *before* parsing, at the file level. Neither
the actor bootstrap engine nor the binary serializer is aware of includes; they
see a single, merged topology document.

**Entrypoint (`main.toml`):**
```toml
[system]
version = "1.0"
imports = [
    "dispatchers.toml",
    "domains/auth/*.toml",
    "domains/network/*.toml"
]
```

**Merge algorithm (pseudocode):**
```python
def load_topology(entrypoint_path):
    base_dir = os.path.dirname(entrypoint_path)
    merged = {"actor": [], "dispatcher": [], "template": {}}

    root = parse_toml(entrypoint_path)
    merged["system"] = root["system"]

    for import_path in root["system"].get("imports", []):
        for file in glob(os.path.join(base_dir, import_path)):
            data = parse_toml(file)
            merged["actor"].extend(data.get("actor", []))
            merged["dispatcher"].extend(data.get("dispatcher", []))
            # Templates: later files cannot override earlier ones
            for name, tmpl in data.get("template", {}).items():
                if name not in merged["template"]:
                    merged["template"][name] = tmpl

    # Root file actors/dispatchers are appended last
    merged["actor"].extend(root.get("actor", []))
    merged["dispatcher"].extend(root.get("dispatcher", []))

    return merged
```

**Design decisions:**
- Glob patterns (`*.toml`) are supported for convenience with large-scale configs
- Duplicate `template` names: first definition wins (imported files cannot override templates from the entrypoint)
- Duplicate `actor.id`: detected at merge time, produces a clear error listing both files
- Imported files do NOT declare their own `[system]` or `imports` — only the entrypoint contains system config

---

## Template Inheritance (Mixin)

### Resolution Algorithm

Templates provide default values. Actor definitions override them. Resolution uses deep merge:

```python
def resolve_actor(actor_def, templates):
    if "inherits" in actor_def:
        base = deep_copy(templates[actor_def["inherits"]])
        deep_merge(base, actor_def)  # actor_def values win
        return base
    return actor_def
```

**Deep merge semantics:**
- Scalar fields (`behavior`, `mailbox_capacity`): actor value replaces template value
- Table fields (`resources`, `args`): merged key-by-key; actor keys override template keys
- If actor specifies `behavior` and template also specifies `behavior`, the actor's value wins
- Inheritance is single-parent (no multiple inheritance). An actor can specify at most one `inherits`

### Example Resolution

```toml
[template.base]
behavior = "Worker"
mailbox_capacity = 4096
dispatch_policy = "Cooperative"
  [template.base.args]
  pool = "default"

[[actor]]
id = "w1"
inherits = "base"
  [actor.args]
  pool = "gpu"       # Override
  timeout = "30"      # New key
```

Resolved result for `w1`:
```yaml
behavior: Worker
mailbox_capacity: 4096
dispatch_policy: Cooperative
args:
  pool: gpu          # Overridden
  timeout: 30         # Merged
```

---

## AOT Compilation Path

### Overview

For production deployments, a build-time tool compiles TOML topology files into
HPActor's custom binary topology format. At startup, the C++ layer `mmap`s this
binary and reconstructs the `TopologyModel` without re-running the TOML parser.
The format is defined in `include/hpactor/config/binary_format.hpp`, written by
`src/config/binary_serializer.cpp`, and read by `src/config/binary_loader.cpp`.

### Binary Topology Format

```cpp
constexpr uint32_t TOPOLOGY_BINARY_MAGIC = 0x48504154; // "HPAT"

struct BinaryHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t system_offset;
    uint32_t dispatcher_count;
    uint32_t dispatchers_offset;
    uint32_t actor_count;
    uint32_t actors_offset;
    uint32_t string_table_offset;
    uint32_t string_table_size;
};

struct BinarySystemDef {
    uint32_t scheduler_threads;
    uint32_t max_queue_depth;
    uint32_t default_mailbox_size;
    uint32_t enable_network;
    uint16_t tcp_port;
    uint16_t reserved_pad;
    uint32_t spawn_timeout_ms;
    uint32_t enable_http_gateway;
    uint16_t http_port;
    uint32_t http_max_connections;
    uint32_t http_max_request_size;
    uint32_t http_reply_timeout_ms;
    uint32_t use_coroutines;
    uint32_t version_offset;
    uint32_t http_bind_host_offset;
};

struct BinaryActorDef {
    uint32_t id_offset;
    uint32_t behavior_offset;
    uint32_t supervisor_offset;
    uint32_t dispatcher_offset;
    uint8_t dispatch_policy;
    uint32_t mailbox_capacity;
    uint32_t slab_class_bytes;
    uint32_t max_memory_kb;
    uint16_t args_count;
    uint32_t args_offset;
};
```

**Key design points:**
- Strings are offsets into one string table, so mapped topology strings can be
  read directly from the binary region.
- Actors are pre-sorted by the compiler; the loader iterates sequentially.
- Fixed-size records keep loader code simple and avoid schema-generator
  dependencies.
- The binary currently captures the core topology fields. New subsystem
  configuration must be added deliberately to the serializer/loader when it
  becomes part of the AOT startup contract.

### C++ Zero-Copy Loading

```cpp
#include <hpactor/config/binary_loader.hpp>

TopologyModel BootstrapEngine::load_from_binary(const std::string& path) {
    return BinaryLoader::load(path).value();
}
```

### AOT Compiler Tool

The AOT compiler is a standalone C++ executable built as part of the CMake project. It lives in `tools/toml-compiler/` and links against `hpactor_lib` to reuse the same TOML parsing, validation, and topological sort logic from Phase 3.

**Technology selection — C++:**

The compiler shares the `toml++` dependency and `TomlParser` merge/validate/sort
logic with the runtime path. The only difference is the final output step:
runtime produces a `TopologyModel` in memory; the AOT compiler serializes that
model with `BinarySerializer` and writes a `.bin` file. Using C++ eliminates an
external language dependency and keeps the AOT and runtime paths aligned.

**File structure:**
```
tools/toml-compiler/
├── CMakeLists.txt               # Builds hpactor_toml_compiler executable
└── compiler.cpp                 # CLI entry point using BinarySerializer

src/config/
├── binary_serializer.cpp        # TopologyModel → HPActor binary topology
└── binary_loader.cpp            # mmap/read binary topology → TopologyModel
```

**Compiler phases:**
```
Phase 1: Load entrypoint TOML
Phase 2: Recursively resolve imports (glob expansion, DFS traversal)
Phase 3: Merge all [[actor]], [[dispatcher]] arrays
Phase 4: Resolve template inheritance (deep merge)
Phase 5: Validate (duplicate ids, missing behaviors, circular dependencies)
Phase 6: Topological sort (Kahn's algorithm)
Phase 7: BinarySerializer writes HPActor binary topology
Phase 8: Write topology.bin
```

---

## Behavior Registration (Factory Pattern)

### Problem

The TOML file references behaviors by string name (`"TcpGatewayActor"`), but `ActorSystem::spawn<T>()` requires a C++ type. C++ has no built-in reflection.

### Solution: Static Registration Macro

```cpp
// In include/hpactor/core/actor_type_registry.hpp

#define HPACTOR_REGISTER_BEHAVIOR(Name, ActorClass)                       \
    namespace {                                                            \
        [[maybe_unused]] static const bool _hpactor_reg_##ActorClass = [] { \
            hpactor::ActorTypeRegistry::register_factory<ActorClass>(      \
                Name);                                                     \
            return true;                                                   \
        }();                                                               \
    }
```

**Usage in actor implementation:**
```cpp
// In tcp_gateway_actor.cpp
#include <hpactor/core/actor_type_registry.hpp>

HPACTOR_REGISTER_BEHAVIOR("TcpGatewayActor", TcpGatewayActor);
```

The macro creates a static initializer that runs before `main()`, populating the global factory map. By the time the bootstrap engine queries `"TcpGatewayActor"`, the factory already knows how to construct it.

### Factory Interface

```cpp
class ActorTypeRegistry {
  public:
    template <typename T>
    static void register_factory(const std::string& name);

    // Returns a callable that constructs the actor
    std::function<std::shared_ptr<AbstractActor>(ActorContext*, ActorSystem&)> 
    get_factory(const std::string& name) const;

    bool has(const std::string& name) const;
};
```

---

## Argument Deserialization

### Problem

`actor.args` in TOML is a `map<string, string>`. Each actor type needs its constructor arguments deserialized from this map.

### Design

Add a static `from_args()` method to actors that accept configuration:

```cpp
class TcpGatewayActor : public EventBasedActor {
  public:
    // Called by bootstrap engine after construction, before on_activate()
    static result<void> configure_from_args(
        const std::unordered_map<std::string, std::string>& args,
        TcpGatewayActor& actor);
};
```

The bootstrap engine:
1. Constructs the actor via the factory (default constructor or minimal args)
2. If the actor type has `configure_from_args`, calls it with the `args` map
3. The actor validates and applies its own args (which it knows the schema for)

This avoids a centralized arg schema registry. Each actor type owns its own configuration contract.

---

## SystemInit Message

### Purpose

Actors spawned during bootstrapping should not process external traffic until the entire topology is ready. A broadcast `SystemInit` message signals that bootstrap is complete.

### Protocol

```cpp
// System message — TypeTag in reserved range (1-99)
constexpr TypeTag SystemInitTag = TypeTag(6);

struct SystemInitMessage {
    // Empty payload — existence is the signal
};
```

After all actors are spawned:
```cpp
for (auto& actor_id : spawned_ids) {
    TypedMessage init_msg(SystemInitTag);
    system.deliver_local(actor_id, std::move(init_msg));
}
```

Actors gate their readiness on receiving this message:
```cpp
Behavior MyActor::make_behavior() {
    return Behavior{
        [this](const SystemInitMessage&) {
            ready_ = true;
            start_accepting_connections();
        },
        [this](const RequestMsg& req) {
            if (!ready_) { stash(req); return; }  // Queue until ready
            process(req);
        }
    };
}
```

---

## Error Handling

All bootstrap errors are **fail-fast** — detected and reported before any actors are spawned. The system either starts cleanly or not at all.

| Error | Detection Point | Action |
|-------|----------------|--------|
| Duplicate `actor.id` | Merge phase | Report both locations, abort |
| Unknown `behavior` | Validation phase | Report actor id + unknown behavior, list known behaviors |
| Circular `supervisor` dependency | Topological sort | Report cycle path, abort |
| Invalid `dispatch_policy` string | Parse phase | Report actor id + invalid value, list valid values |
| Missing template reference | Template resolution | Report actor id + missing template name |
| File not found (import) | Merge phase | Report import path, abort |

Error messages follow this format:
```
TOML bootstrap error in main.toml:
  actor "auth_worker_1": unknown behavior "AuthActor2"
  Did you mean "AuthActor"?
  Known behaviors: AuthActor, TcpGatewayActor, WorkerActor, EchoActor
```

---

## Implementation Plan

### Phase 1: Schema and Internal Model

| Step | File | Change |
|------|------|--------|
| 1 | `include/hpactor/config/topology_model.hpp` | Define `ActorDef`, `DispatcherDef`, `TopologyModel` structs |
| 2 | `include/hpactor/config/toml_schema.hpp` | Schema constants, valid `dispatch_policy` strings |

### Phase 2: Runtime TOML Parser

| Step | File | Change |
|------|------|--------|
| 3 | `src/config/toml_parser.cpp` | Parse TOML → `TopologyModel`. Implement import resolution, template resolution, topological sort, validation |
| 4 | `CMakeLists.txt` | Add `toml++` dependency (header-only, vendored or fetched) |

### Phase 3: Bootstrap Engine

| Step | File | Change |
|------|------|--------|
| 5 | `src/config/bootstrap_engine.cpp` | `BootstrapEngine::execute()` — dispatcher creation, behavior validation, pre-allocation, spawn loop |
| 6 | `src/actor/actor_system.cpp` | Add `ActorSystem::load_topology(path)` convenience method |

### Phase 4: Factory Macro

| Step | File | Change |
|------|------|--------|
| 7 | `include/hpactor/core/actor_type_registry.hpp` | Add `register_factory<T>()`, `get_factory()`, `has()`, `HPACTOR_REGISTER_BEHAVIOR` macro |

### Phase 5: AOT Compiler (Python)

| Step | File | Change |
|------|------|--------|
| 8 | `tools/toml-compiler/compiler.py` | TOML merge, template resolution, validation, FlatBuffers serialization |
| 9 | `tools/toml-compiler/topology.fbs` | FlatBuffers schema |
| 10 | `src/config/flatbuffers_loader.cpp` | `BootstrapEngine::load_from_binary()` — mmap + zero-copy read |

### Phase 6: Integration and Tests

| Step | File | Change |
|------|------|--------|
| 11 | `tests/test_toml_config.cpp` | Unit tests: parse, validate, topological sort, template resolution, duplicate detection |
| 12 | `tests/test_bootstrap_engine.cpp` | Integration tests: spawn tree from TOML, verify supervision, SystemInit delivery |
| 13 | `examples/` | Example TOML configs for common patterns |

---

## Configuration Examples

### Pattern 1: Simple Pipeline

```toml
[[actor]]
id = "ingest"
behavior = "IngestActor"

[[actor]]
id = "transform"
behavior = "TransformActor"
supervisor = "ingest"

[[actor]]
id = "sink"
behavior = "SinkActor"
supervisor = "transform"
```

Topological order: `ingest` → `transform` → `sink`

### Pattern 2: Router with Worker Pool

```toml
[[dispatcher]]
name = "worker_pool"
threads = 8

[template.pool_worker]
behavior = "ComputeWorker"
dispatcher = "worker_pool"

[[actor]]
id = "router"
behavior = "RoundRobinRouter"

[[actor]]
id = "worker_1"
inherits = "pool_worker"
supervisor = "router"

[[actor]]
id = "worker_2"
inherits = "pool_worker"
supervisor = "router"

[[actor]]
id = "worker_3"
inherits = "pool_worker"
supervisor = "router"

[[actor]]
id = "worker_4"
inherits = "pool_worker"
supervisor = "router"

  [actor.args]            # Override per-worker
  gpu_device = "1"
```

### Pattern 3: Multi-Service System

```toml
[system]
version = "1.0"
scheduler_threads = 8
imports = [
    "dispatchers.toml",
    "domains/auth/*.toml",
    "domains/api/*.toml",
    "domains/database/*.toml"
]

# main.toml: only system-level roots
[[actor]]
id = "api_gateway"
behavior = "HttpGatewayActor"
dispatcher = "io_pool"

[[actor]]
id = "db_pool"
behavior = "ConnectionPoolActor"
dispatcher = "io_pool"
```

Sub-files define domain actors as children of these roots.

---

## Extensible Parser Registry

`TomlParser` owns document loading and topology assembly. Subsystems own their
own section parsers and self-register them through file-scope static registrar
objects. There is no public `register_*` API — adding a parser means adding a
source file to the build.

### Parser Categories

Two parser interfaces service different parse scopes:

- **`ITomlSystemConfigParser`** — receives the entrypoint `[system]` table and
  mutates `SystemDef`. Used for subsystem config tables under `[system]`:
  `[system.process]`, `[system.quarantine]`, `[system.ai.accelerators]`,
  `[system.mailbox]`, `[system.tracing]`, `[system.dead_letters]`,
  `[system.delivery]`, `[system.metrics]`, `[system.python]`,
  `[system.logging]`, `[system.shutdown]`, `[system.name_resolution]`,
  `[system.cli]`, `[system.discovery]`, and `[system.transport]`.
- **`ITomlDocumentConfigParser`** — receives each TOML document root and appends
  dispatchers, templates, or actors to `TomlFileData`. Used for
  `[[dispatcher]]`, `[template.*]`, and `[[actor]]`.

### Built-in Parsers

| Parser | Category | Order | Source |
|--------|----------|-------|--------|
| `system.core` | System | 0 | `src/config/parsers/system_core_config_parser.cpp` |
| `topology.document` | Document | 0 | `src/config/parsers/topology_config_parser.cpp` |
| `system.process` | System | 5 | `src/config/parsers/process_config_parser.cpp` |
| `system.quarantine` | System | 10 | `src/config/parsers/quarantine_parser.cpp` |
| `system.ai.accelerators` | System | 15 | `src/config/parsers/ai_accelerator_config_parser.cpp` |
| `system.mailbox` | System | 20 | `src/config/parsers/mailbox_config_parser.cpp` |
| `system.tracing` | System | 25 | `src/config/parsers/tracing_config_parser.cpp` |
| `system.dead_letters` | System | 30 | `src/config/parsers/dead_letters_config_parser.cpp` |
| `system.delivery` | System | 35 | `src/config/parsers/delivery_config_parser.cpp` |
| `system.metrics` | System | 40 | `src/config/parsers/metrics_config_parser.cpp` |
| `system.python` | System | 45 | `src/config/parsers/python_binding_config_parser.cpp` |
| `system.logging` | System | 50 | `src/config/parsers/logging_config_parser.cpp` |
| `system.shutdown` | System | 55 | `src/config/parsers/shutdown_config_parser.cpp` |
| `system.name_resolution` | System | 60 | `src/config/parsers/name_resolution_config_parser.cpp` |
| `system.cli` | System | 65 | `src/config/parsers/cli_config_parser.cpp` |
| `system.discovery` | System | 70 | `src/config/parsers/discovery_config_parser.cpp` |
| `system.transport` | System | 75 | `src/config/parsers/transport_outbound_config_parser.cpp` |

A single orphaned parser (`ask_config_parser.cpp`, order 85) exists on disk but
is not yet compiled — it needs `SystemDef` field additions before it can be
wired into the build.

Three parsers were removed as dead stubs pending future implementation:
`rate_limiting_config_parser.cpp` (deferred to Phase 8),
`passivation_config_parser.cpp` (read keys but discarded all values), and
`cluster_leadership_parser.cpp` (read keys but discarded all values).

### Static Self-Registration

Each parser translation unit owns a file-scope registrar object:

```cpp
namespace {

class MetricsConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.metrics";
    static constexpr int kOrder = 100;

    std::string_view name() const noexcept override { return kName; }
    int order() const noexcept override { return kOrder; }

    result<void> parse(const TomlTableView& system,
                       SystemDef& out,
                       TomlParseContext& ctx) const override;
};

const TomlSystemParserRegistration<MetricsConfigParser>
    kRegisterMetricsConfigParser;

} // namespace
```

Registrars run before `main()`. The registry sorts parsers by `order()` then
`name()`, so parse order is independent of static initialization order across
translation units.

### TOML Adapter Isolation

Parser interfaces do not include `toml.hpp`. An opaque `TomlTableView` wraps
`toml::table*` and is implemented in `src/config/toml_table_view.cpp` — one of
only two translation units compiled with `-fexceptions`. Subsystem parsers are
compiled with `-fno-exceptions` and never see toml++ types.

### Adding a New Subsystem Parser

1. Create a parser source file in `src/config/parsers/`.
2. Implement `ITomlSystemConfigParser` or `ITomlDocumentConfigParser`.
3. Add a file-scope `TomlSystemParserRegistration<T>` or
   `TomlDocumentParserRegistration<T>` object.
4. Add the source to `CMakeLists.txt`.

No edits to `parse_file_data` or `TomlParser` are needed.

---
## Open Questions

1. **Should `actor.args` support typed values?** Currently all values are strings. Typed args (int, bool, float) would require a tagged union in the schema. Recommendation: start string-only; add typed args if demand arises.

2. **Hot-reload of topology?** The initial design covers startup only. Hot-reloading (changing the actor tree at runtime) requires diff-based update of the running topology — significantly more complex. Defer to a future design.

3. **Should the bootstrap engine be an actor itself?** As a system actor, it could receive spawn responses as messages. However, bootstrap is inherently synchronous (must complete before accepting external traffic). A non-actor engine is simpler and sufficient.

4. **Integration with existing `spawn_remote()`?** The TOML config describes a single node's topology. Cross-node spawning (placing actors on remote nodes) is a future extension. The `dispatcher` name could map to a remote node name.

5. **TOML vs. FlatBuffers binary versioning?** The `system.version` field must match the FlatBuffers schema version. The loader checks this and rejects mismatched binaries with a clear error.
