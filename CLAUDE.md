# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Configure and build
cmake -S . -B build -GNinja
ninja -C build

# Run tests
ctest --output-on-failure

# Run a single test
./build/tests/test_<name>

# Build with sanitizers
cmake -DENABLE_TSAN=ON ..  # ThreadSanitizer
cmake -DENABLE_ASAN=ON ..  # AddressSanitizer

# Build options
cmake -DENABLE_EXAMPLES=OFF ..    # Disable examples (default ON)
cmake -DENABLE_PROACTOR=ON ..     # Enable proactor backend
cmake -DENABLE_MEMORY_DEBUG=ON .. # Enable memory poisoning + canary verification
cmake -DENABLE_ACTOR_METRICS=OFF .. # Disable actor-level metrics (default ON)
cmake -DENABLE_CLI=OFF ..       # Disable interactive CLI subsystem (default ON, runtime opt-in via cli.enabled)
```

## Architecture

HPActor is a C++20 event-based actor framework inspired by CAF (C++ Actor Framework).

### Production Reliability Direction

The current architecture roadmap is organized around a production reliability
plane for 24x7 distributed actor operation. The primary entry point is
`docs/architecture/production/production-reliability-plane.md`, with a refined
feature-gap backlog in
`docs/architecture/production/feature-gap-refined-requirement-backlog.md`.

When adding production-facing actor-system features, align the design with these
planes:

- **Data plane**: delivery semantics, mailbox admission, DLQ, reliable
  messaging, tracing, actor lifecycle.
- **Control plane**: cluster failure model, node identity, sharding, placement,
  rebalancing, graceful shutdown, rolling upgrades.
- **Operations plane**: health, admin API, security, audit, config reload,
  incident timelines, chaos/soak/fuzz testing.

### Actor Type Hierarchy

```
AbstractActor (interface base)
    └── LocalActor (has ActorContext access)
            ├── EventBasedActor (cooperative, behavior-based, proto handlers)
            │       ├── StatefulActor<T> (explicit state)
            │       ├── ProtoStatefulActor<T> (protobuf-native + explicit state)
            │       ├── SpawnReceiver (system actor, handles remote spawn)
            │       └── DenseComputingActor (dedicated pool dispatch)
            ├── TypedEventBasedActor<Signatures...> (statically typed)
            ├── BlockingActor (thread-based, blocking receive)
            │       └── ScopedActor (for main/non-actor contexts)
            └── DaemonActor (dedicated thread, run_once() loop)
                    ├── PollingActor (CPU affinity, poll budget)
                    ├── ExternalMsgGatewayActor (named route table, message transforms)
                    │       └── net::HTTPGatewayActor (HTTP ingress gateway)
                    └── cli::CliActor (stdin/socket I/O, command tree, InspectState requests)
```

### Key Components

| Component | Location | Purpose |
|-----------|----------|---------|
| `ActorSystem` | `core/actor_system.hpp` | Actor environment, spawn, registry, clock, topology loading |
| `ActorContext` | `actor_context.hpp` | Actor execution context, message sending |
| `Behavior` | `behavior.hpp` | Message handler with `become()` support |
| `TypedBehavior` | `actor/typed_actor.hpp` | Statically typed handlers |
| `Supervision` | `supervision/*.hpp` | Fault-tolerance (OneForOne, AllForOne, self-supervising) |
| `HybridScheduler` | `sched/hybrid_scheduler.hpp` | Work-stealing scheduler with A2WS victim selection |
| `MPSCActorMailbox` | `mailbox/mpsc_actor_mailbox.hpp` | Lock-free MPSC queue with edge-triggered CAS wakeup |
| `SlabCache` / `SegmentProvider` | `mem/*.hpp` | Two-tier slab allocator (thread-local caches + mmap segments) |
| `EventLoop` | `net/event_loop.hpp` | kqueue/epoll edge-triggered event loop |
| `TomlParser` | `config/toml_parser.hpp` | TOML topology parser: coordinator that delegates to self-registered subsystem parsers |
| `TomlParserRegistry` | `config/toml_parser_registry.hpp` | IoC registry for TOML subsystem parsers with static self-registration |
| `TomlTableView` | `config/toml_table_view.hpp` | Opaque TOML table wrapper isolating toml.hpp from parser interfaces |
| `TopologyModel` | `config/topology_model.hpp` | System topology (actors, dispatchers, system config) |
| `ActorFactoryRegistry` | `config/actor_factory_registry.hpp` | Maps behavior name strings to actor factory functions |
| `BinaryLoader` / `BinarySerializer` | `config/binary_*.hpp` | mmap-friendly binary topology format |
| `MetricsActor` / `MpscRingBuffer` | `metrics/*.hpp` | Lock-free ring buffer instrumentation, OpenMetrics /metrics endpoint for Prometheus |
| `CliActor` / `CommandNode` | `cli/*.hpp` | Interactive CLI with trie-based command tree, InspectState introspection, paged output |
| `IServiceDiscovery` | `net/service_discovery.hpp` | Pluggable discovery interface (UdpRegistrar, Gossip, Static, Hybrid) |
| `GossipMembership` | `net/gossip_membership.hpp` | SWIM protocol for decentralized cross-server discovery |
| `ActorLocationCache` | `net/actor_location_cache.hpp` | TTL cache for ActorId → EndPoint resolution |

### Production Architecture Backlog

The production reliability docs are architecture requirements, not implemented
runtime features yet. Key files:

| Document | Purpose |
|----------|---------|
| `docs/architecture/production/production-reliability-plane.md` | Top-level 24x7 reliability roadmap |
| `docs/architecture/production/architecture-requirement-backlog.md` | Summary production requirement backlog |
| `docs/architecture/production/feature-gap-refined-requirement-backlog.md` | Detailed subsystem requirement cards |
| `docs/architecture/production/actor-delivery-semantics-design.md` | Delivery results, TTL, retry, duplicate semantics |
| `docs/architecture/production/dead-letter-queue-design.md` | Dead-letter record, retention, replay, observability |
| `docs/architecture/production/cluster-failure-model-design.md` | Node state, partition, quarantine, fencing model |
| `docs/architecture/production/cluster-sharding-placement-design.md` | Shards, placement, handoff, route invalidation |
| `docs/architecture/production/security-architecture-design.md` | mTLS, authorization, audit, secret handling |
| `docs/architecture/production/operations-sre-design.md` | Health, admin API, incident timeline, SLO signals |
| `docs/architecture/production/chaos-reliability-testing-design.md` | Fault injection, chaos, soak, fuzz, compatibility tests |

### Message Flow

Actors communicate via protobuf `TypedMessage` (TypeTag + payload). From within an actor:
- `context()->send(addr, msg)` — send message
- `context()->reply(msg)` — reply to sender
- `context()->reply_with_error(code)` — reply with error
- `become(Behavior)` — change behavior dynamically
- `co_await mailbox_awaiter` — suspend until message arrives (coroutine actors)

Proto actors register handlers declaratively:
- `on<T>(handler)` — fire-and-forget proto handler
- `on_request<ReqT, ResT>(handler)` — request-response proto handler

### Topology Configuration

The TOML config topology system bootstraps actor trees from declarative config:

```
ActorSystem::load_topology("config.toml")  // parse + bootstrap
```

Pipeline: TOML file(s) → `TomlParser::parse()` (coordinator) → registered subsystem parsers → `TopologyModel` → `BootstrapEngine::execute()` → spawned actors. The `tools/toml-compiler/` AOT compiler pre-compiles TOML to a compact binary format for production (zero-parse mmap bootstrap).

Subsystem parsers self-register via file-scope static registrar objects (`TomlSystemParserRegistration<T>` / `TomlDocumentParserRegistration<T>`). New TOML subsystem config is added by creating a parser source file in `src/config/parsers/` with a registrar — no edits to `parse_file_data` needed. Parser interfaces use opaque `TomlTableView` so no public header includes `toml.hpp`.

### Supervision

- `OneForOneSupervisor` — only failed child restarts
- `AllForOneSupervisor` — all children restart when one fails
- `SupervisorActor` — supervises children via strategy pattern
- `SelfSupervisingActor` — manages own children with policy (max restarts, interval)

### Design Constraints

- C++20, no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`)
- Header-only types + compiled runtime (hpactor_lib)
- LLVM coding standards (see CMakeLists.txt compiler flags)
- `-fexceptions` enabled only for `src/config/toml_parser.cpp`, `src/config/toml_table_view.cpp`, and `tools/toml-compiler/compiler.cpp` (toml++ requires exceptions in including TUs)
- System packages: OpenSSL, Protobuf; vendored: llhttp, toml++ v3.4.0 (single-header in `third_party/`)

## Important Files

- `include/hpactor/` — public headers (actor, cli, config, core, mailbox, metrics, mem, net, ref, rpc, sched, spawn, supervision, types)
- `src/` — implementation files (linked into hpactor_lib)
- `tests/` — 99 unit tests
- `examples/` — 9 API usage examples
- `tools/toml-compiler/` — AOT TOML-to-binary compiler
- `third_party/` — vendored dependencies (llhttp, toml++)
- `cmake/` — CMake modules (protobuf codegen, toml++ interface target)
- `docs/architecture/production/` — production reliability plane, missing design docs, and refined requirement backlog
- `docs/superpowers/tutorials/actor-framework-tutorial.md` — usage guide
- `.claude/projects/*/memory/` — persistent project memory
