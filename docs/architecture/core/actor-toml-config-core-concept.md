# Actor TOML Configuration — Core Concept

## Overview

HPActor's TOML configuration system provides **declarative topology bootstrapping** — you describe the entire actor tree in a TOML file, and the Actor System reads it at startup to spawn all actors in the correct order. This is analogous to Docker Compose or Kubernetes YAML: a "blueprint" drives a bootstrap engine that resolves dependencies, pre-allocates resources, and batch-spawns the actor hierarchy.

**Core Principle:** The startup topology is data, not code. Separating "what actors exist" from "how each actor behaves" keeps the system composable, auditable, and reconfigurable without recompilation.

---

## Problem Statement

Today, HPActor actors are spawned programmatically via `ActorSystem::spawn<T>(Args...)` or `ActorContext::spawn()`. While this works, it means:

1. **Topology is embedded in C++** — Changing the actor tree requires modifying and recompiling source code.
2. **No global startup overview** — The full system graph is scattered across `main()` and various initialization functions.
3. **Order dependencies are implicit** — If a Router must start before its Workers, this constraint lives only in code comments.
4. **No reuse of topology patterns** — A common pattern (e.g., "1 supervisor + N workers behind a router") must be hand-coded each time.

A declarative configuration solves all four: the topology is a single, inspectable artifact that the framework interprets at startup.

---

## Philosophy

### Declarative, Not Imperative

```
// Imperative (current): HOW to build the tree
auto router = system.spawn<RouterActor>(pool_size);
auto worker1 = system.spawn<WorkerActor>(router->address());
auto worker2 = system.spawn<WorkerActor>(router->address());
router->add_route(worker1);
router->add_route(worker2);

// Declarative (TOML): WHAT the tree looks like
[[actor]]
id = "router"
behavior = "RouterActor"

[[actor]]
id = "worker_1"
behavior = "WorkerActor"
supervisor = "router"

[[actor]]
id = "worker_2"
behavior = "WorkerActor"
supervisor = "router"
```

### Two-Path Design

| Path | When | Mechanism | Overhead |
|------|------|-----------|----------|
| **Runtime** | Development, small deployments | Parse TOML directly at startup | One-time string allocation |
| **AOT (Ahead-of-Time)** | Production, large deployments | Compile TOML→FlatBuffers at build time, `mmap` at startup | Zero-copy, zero-allocation |

Both paths produce the same in-memory topology representation. The runtime path uses a TOML parser (e.g., `toml++`); the AOT path uses a pre-compiled FlatBuffers binary. The bootstrap engine is identical downstream.

### Integration with Existing Architecture

The TOML config system wraps — not replaces — the existing spawn infrastructure:

```
TOML File
    │
    ▼
Bootstrap Engine (NEW)
    │
    ├─ Parses TOML / mmaps FlatBuffers
    ├─ Resolves DAG spawn order
    ├─ Validates against ActorTypeRegistry
    │
    ▼
ActorSystem::spawn<T>(...)  ← Existing API, unchanged
```

Actors defined in TOML are spawned through the same `ActorSystem::spawn<T>()` template path used by programmatic spawns today. The bootstrap engine is a consumer of the existing API.

---

## TOML by Example

### Minimal System

```toml
[system]
version = "1.0"
default_mailbox_size = 1024

[[actor]]
id = "echo_server"
behavior = "EchoActor"
```

This defines a single `EchoActor` instance. At startup, the bootstrap engine:
1. Looks up `"EchoActor"` in the `ActorTypeRegistry`
2. Calls `ActorSystem::spawn<EchoActor>()`
3. Registers the actor under the name `"echo_server"` in `actor_registry`

### Multi-Actor Supervision Tree

```toml
[system]
version = "1.0"
scheduler_threads = 4
default_mailbox_size = 1024

# Dispatcher pools map to scheduler threads and CPU affinity
[[dispatcher]]
name = "io_pool"
threads = 2
cpu_affinity = [0, 1]

[[dispatcher]]
name = "compute_pool"
threads = 4
cpu_affinity = [2, 3, 4, 5]

# Actor definitions form the supervision tree
[[actor]]
id = "tcp_gateway"
behavior = "TcpGatewayActor"
dispatcher = "io_pool"
dispatch_policy = "DedicatedThread"
mailbox_capacity = 4096

  [actor.args]
  listen_port = "8080"
  protocol = "IPv4"

[[actor]]
id = "auth_worker_1"
behavior = "AuthActor"
dispatcher = "compute_pool"
supervisor = "tcp_gateway"

[[actor]]
id = "auth_worker_2"
behavior = "AuthActor"
dispatcher = "compute_pool"
supervisor = "tcp_gateway"
```

### Template Inheritance

When many actors share the same base configuration, use `[template]`:

```toml
[template.base_worker]
behavior = "ComputeWorker"
dispatcher = "compute_pool"
mailbox_capacity = 4096
dispatch_policy = "Cooperative"

  [template.base_worker.resources]
  slab_class_bytes = 256
  max_memory_kb = 1024

[[actor]]
id = "worker_001"
inherits = "base_worker"

[[actor]]
id = "worker_002"
inherits = "base_worker"
mailbox_capacity = 8192   # Override template default
```

Templates are resolved at compile time (AOT) or parse time (runtime). The compiler performs a deep merge: template values are defaults, overridden by per-actor settings.

---

## Configuration Dimensions

A TOML topology describes actors across five dimensions:

| Dimension | TOML Section | Maps To |
|-----------|-------------|---------|
| **Identity** | `actor.id`, `actor.behavior` | `ActorId`, `ActorType`, factory lookup |
| **Placement** | `actor.dispatcher`, `actor.dispatch_policy` | `DispatchPolicy`, `DispatchHints` |
| **Hierarchy** | `actor.supervisor` | Parent-child relationship, supervision tree |
| **Resources** | `actor.resources`, `actor.mailbox_capacity` | Mailbox sizing, slab allocation |
| **Parameters** | `actor.args` (key-value dict) | Constructor arguments, deserialized per-actor |

---

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| TOML over YAML/JSON | Flat structure maps cleanly to C++ structs; strong typing; no significant-whitespace pitfalls |
| TOML over custom DSL | Standard tooling (editors, linters, schema validators); zero learning curve |
| Separate bootstrap engine | Clean separation: config is data, spawn is mechanism. Engine replaces hand-written `main()` init |
| Optional AOT compilation | Development stays fast (parse text); production stays lean (mmap binary). Same engine, two front-ends |
| Template inheritance | Avoids repetition in large topologies. Deep-merge semantics keep overrides explicit |
| DAG-based spawn order | Ensures supervisors exist before children without manual ordering |

---

## Relationship to Existing Components

| Existing Component | Role in TOML Config |
|-------------------|---------------------|
| `ActorSystem::spawn<T>()` | Called by bootstrap engine for each actor in the topology |
| `ActorTypeRegistry` | Resolves `behavior` strings to C++ types, validates that all referenced behaviors are registered |
| `actor_registry` | Populated with `actor.id` → `ActorAddress` mappings after spawn |
| `Config` struct | Absorbs `[system]` section values (threads, ports, timeouts) |
| `DispatchPolicy` / `DispatchHints` | Mapped from `dispatcher` tables and per-actor `dispatch_policy` |
| Supervision tree | Built from `supervisor` field — the engine calls `context()->spawn()` on the parent, not `ActorSystem::spawn()` |
| `ProtoTypeRegistry` | Referenced if actors use protobuf message types (TypeTag resolution) |

---

## File Organization Convention

For systems with many actors, split configs by domain:

```text
/config
├── main.toml              # Entrypoint, [system] section, imports list
├── templates.toml          # Shared [template] definitions
├── dispatchers/
│   └── pools.toml          # [[dispatcher]] definitions
└── domains/
    ├── auth/
    │   ├── authenticators.toml
    │   └── session_managers.toml
    └── network/
        ├── gateways.toml
        └── routers.toml
```

The `main.toml` declares imports; the bootstrap engine (or AOT compiler) merges all files into a single topology before spawning.

---

## Limitations

1. **TOML has no native `import`** — The official TOML spec does not support includes. The runtime parser and AOT compiler both implement this as a preprocessor step (see architecture doc).
2. **Behavior lookup is string-based** — Requires `ACTOR_REGISTER_BEHAVIOR` macros to populate the factory registry. Missing registrations are caught at startup (fail-fast), not compile time.
3. **Dynamic topology changes are out of scope** — The TOML config describes the *initial* topology. Actors spawned at runtime (e.g., per-connection workers) use the programmatic API as before.
4. **Not a replacement for `ActorContext::spawn()`** — Child actors created in response to messages (dynamic children) still use the imperative API. TOML covers static topology only.
