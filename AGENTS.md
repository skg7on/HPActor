# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Session Warmup

At the start of a substantive task, read these files first:

1. `AGENTS.md` — Codex-facing working instructions for this repo.
2. `CLAUDE.md` — parallel Claude-facing instructions; keep shared build and architecture guidance in sync when it changes.
3. `CLAUDE_MEMORY.md` — project memory summary with current feature status, implementation history, docs, and recent test counts.

Treat `CLAUDE_MEMORY.md` as the high-level project memory source in this checkout. If persistent memory directories are introduced later, add their exact path here instead of relying on wildcard paths.

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

## Current Project State

The project memory currently records the following major completed areas:

- Actor core framework through unified message passing and zero-copy reactor read path.
- Scheduling subsystem: Chase-Lev work stealing, priority queues, EDF queue, A2WS, timing wheel, coroutine frame pool, hybrid scheduler, worker threads, awaiters, and MPSC mailbox.
- Network layer: kqueue/epoll event loop, optional TLS/plain TCP transport, connection pooling, UDS support, async UDP, registrar protobuf serialization, and reactor/proactor separation.
- Remote actor spawn and async RPC channel, including spawn serialization and response correlation.
- Memory management: mmap-backed segment provider, thread-local slab caches, allocator telemetry, per-actor tracking, poisoning/canaries, hibernation, compaction, and ZRAM hints.
- TOML topology configuration: parser, imports/templates, factory registry, bootstrap engine, SystemInit broadcast, binary format, and AOT compiler.
- Actor metrics: lock-free metric event ring buffer, registry, aggregator, OpenMetrics formatter, MetricsActor, and TOML metrics config.
- Interactive CLI: trie command registry, lexer, output formatters, pager, inspect/kill/list/stats protobuf requests, and runtime opt-in config.
- Pluggable service discovery: `IServiceDiscovery`, `UdpRegistrar`, SWIM `GossipMembership`, `HybridDiscovery`, `StaticDiscovery`, and `ActorLocationCache`.
- Tests: project memory reports 99 unit tests passing.

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
| `TomlParser` | `config/toml_parser.hpp` | TOML topology parser with imports, templates, DAG validation |
| `TopologyModel` | `config/topology_model.hpp` | System topology (actors, dispatchers, system config) |
| `ActorFactoryRegistry` | `config/actor_factory_registry.hpp` | Maps behavior name strings to actor factory functions |
| `BinaryLoader` / `BinarySerializer` | `config/binary_*.hpp` | mmap-friendly binary topology format |
| `MetricsActor` / `MpscRingBuffer` | `metrics/*.hpp` | Lock-free ring buffer instrumentation, OpenMetrics /metrics endpoint for Prometheus |
| `CliActor` / `CommandNode` | `cli/*.hpp` | Interactive CLI with trie-based command tree, InspectState introspection, paged output |
| `IServiceDiscovery` | `net/service_discovery.hpp` | Pluggable discovery interface (UdpRegistrar, Gossip, Static, Hybrid) |
| `GossipMembership` | `net/gossip_membership.hpp` | SWIM protocol for decentralized cross-server discovery |
| `ActorLocationCache` | `net/actor_location_cache.hpp` | TTL cache for ActorId → EndPoint resolution |

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

Pipeline: TOML file(s) → `TomlParser::parse()` → `TopologyModel` → `BootstrapEngine::execute()` → spawned actors. The `tools/toml-compiler/` AOT compiler pre-compiles TOML to a compact binary format for production (zero-parse mmap bootstrap).

### Supervision

- `OneForOneSupervisor` — only failed child restarts
- `AllForOneSupervisor` — all children restart when one fails
- `SupervisorActor` — supervises children via strategy pattern
- `SelfSupervisingActor` — manages own children with policy (max restarts, interval)

### Design Constraints

- C++20, no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`)
- Header-only types + compiled runtime (hpactor_lib)
- LLVM coding standards (see CMakeLists.txt compiler flags)
- `-fexceptions` enabled only for `src/config/toml_parser.cpp` and `tools/toml-compiler/compiler.cpp` (toml++ requires exceptions in including TUs)
- System packages: OpenSSL, Protobuf; vendored: llhttp, toml++ v3.4.0 (single-header in `third_party/`)

## Important Files

- `include/hpactor/` — public headers (actor, cli, config, core, mailbox, metrics, mem, net, ref, rpc, sched, spawn, supervision, types)
- `src/` — implementation files (linked into hpactor_lib)
- `tests/` — 99 unit tests
- `examples/` — 9 API usage examples
- `tools/toml-compiler/` — AOT TOML-to-binary compiler
- `third_party/` — vendored dependencies (llhttp, toml++)
- `cmake/` — CMake modules (protobuf codegen, toml++ interface target)
- `docs/superpowers/tutorials/actor-framework-tutorial.md` — usage guide
- `CLAUDE_MEMORY.md` — current project memory summary for this checkout
