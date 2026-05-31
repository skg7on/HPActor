# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Session Warmup

At the start of a substantive task, read these files first:

1. `AGENTS.md` — Codex-facing working instructions for this repo.
2. `CLAUDE.md` — parallel Claude-facing instructions; keep shared build and architecture guidance in sync when it changes.
3. `CLAUDE_MEMORY.md` — project memory summary with current feature status, implementation history, docs, and recent test counts.

Treat `CLAUDE_MEMORY.md` as the high-level project memory source in this checkout. If persistent memory directories are introduced later, add their exact path here instead of relying on wildcard paths.

## Required Worktree Workflow

Every design or implementation job must happen in an isolated git worktree under
the repository-local `.worktrees/` directory.

- Before writing a design/spec/plan or changing source, docs, config, tests, or
  build files, detect whether the current checkout is already a linked worktree.
- If not already in a linked worktree, create one at `.worktrees/<short-task-name>`
  on a task-specific branch, then do all edits there.
- Do not create new worktrees under `.worktree/`; that legacy directory may
  exist locally, but `.worktrees/` is the project convention.
- Keep `.worktrees/` ignored. If the ignore rule is missing, add it before
  creating a project-local worktree.
- Use the worktree's own `build/` directory for configure/build/test output.
- Pure read-only inspection may happen from the main checkout, but any design or
  implementation write must move into `.worktrees/` first.

## Build Commands

```bash
# Configure and build
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
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
cmake -DENABLE_APPS=OFF ..        # Disable complex demo applications (default ON)
cmake -DENABLE_PROACTOR=ON ..     # Enable proactor backend
cmake -DENABLE_MEMORY_DEBUG=ON .. # Enable memory poisoning + canary verification
cmake -DENABLE_ACTOR_METRICS=OFF .. # Disable actor-level metrics (default ON)
cmake -DENABLE_CLI=OFF ..       # Disable interactive CLI subsystem (default ON, runtime opt-in via cli.enabled)
cmake -DENABLE_CLANG_TIDY=ON .. # Enable clang-tidy checks during C++ builds (default OFF)
```

## Build Verification Discipline

After code modifications, do not rebuild the whole project by default. Prefer
the narrowest verification that covers the changed surface, such as a targeted
`ninja` target, one test binary, or `ctest -R <pattern> --output-on-failure`.
Run a full configure/build/test cycle only when it is necessary because the
change affects shared build configuration, generated files, broad public
headers, cross-cutting runtime behavior, or when the user explicitly asks for
full-project verification.

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

### Agent Operating Rules

- Treat the production architecture docs as requirements and design backlog
  until code proves otherwise; do not describe backlog items as implemented
  runtime behavior.
- Start design work from the relevant architecture doc in
  `docs/architecture/production/`, then capture runtime contracts, failure
  semantics, observability, and acceptance evidence before implementation.
- Preserve source-compatible defaults for existing actor APIs. Production-grade
  behavior such as delivery results, bounded mailboxes, reliable messaging,
  tracing, security, and durability should be opt-in or safely defaulted.
- Keep actor boundaries explicit: use protobuf `TypedMessage` type tags for
  dynamic messages, typed actor signatures for static contracts, and avoid
  shared mutable state between actors.
- Prefer subsystem-owned extension points over central switches. New TOML
  subsystem config should use self-registering parsers and opaque
  `TomlTableView`, not public `toml++` headers or edits that grow a monolithic
  parser.
- For production-facing changes, include the operations surface in the same
  design: metrics, CLI/admin visibility, health/readiness, audit or trace
  correlation, and runbook impact when applicable.

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
- Deterministic fault injection: `FaultController`, `FaultSchedule`, `FaultPoint` registry, 14-domain tick counters, seed-replayable schedules with 80 injection sites.
- Mailbox architecture: `MultiLaneQueue<T>` lock-free multi-lane queue with dedicated system lane, priority-aware routing, `DropLowestPriority` overflow, and per-lane depth observability.
- DLQ CLI: `/dlq list`, `/dlq show`, `/dlq replay`, and `/dlq export` commands with `DeadLetterQueue` API.
- Scheduler hardening: `ActorExecutionEngine`, `WorkerThread` fixes, `IScheduler`/`IWorkPlacementScheduler`/`IWorkerNotification` interface segregation.
- Complex apps: EdgeOps telemetry platform and order platform under `apps/`.
- Tests: 1411 source-level GTest cases across 219 test source files and 32 GTest binaries in a three-tier (unit/integration/system) structure.

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

Subsystem parsers self-register via file-scope static registrar objects. New TOML subsystem config is added by creating a parser source file in `src/config/parsers/` — no edits to `parse_file_data` needed. Parser interfaces use opaque `TomlTableView` so no public header includes `toml.hpp`.

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

### Implementation Constraints

- Do not introduce `dynamic_cast`, `typeid`, exception-based control flow, or
  public APIs that require RTTI/exceptions.
- Keep blocking I/O and long-running work out of event-loop and cooperative
  scheduler paths; use the existing daemon, blocking, dense-compute, or async
  abstractions where appropriate.
- Maintain memory-accounting and allocator ownership rules when adding queues,
  envelopes, buffers, or actor state. Bounded capacity and explicit failure
  paths are preferred over unbounded growth.
- When changing lock-free, scheduler, mailbox, timer, or transport code, state
  the concurrency contract in the design and add focused stress or race-oriented
  tests where practical.
- Keep generated/protobuf contracts and TypeTag assignments explicit and
  backward-aware. Compatibility checks are required for protocol, binary
  topology, or persisted-state changes.
- Tests should match risk: narrow unit tests for local behavior, integration
  tests for actor/network/config interactions, and sanitizer/chaos/soak coverage
  for reliability-plane features.

## Important Files

- `include/hpactor/` — public headers (actor, cli, config, core, fault, mailbox, metrics, mem, net, ref, rpc, sched, spawn, supervision, types)
- `src/` — implementation files (linked into hpactor_lib)
- `tests/` — 219 test source files across unit, integration, and system tiers; 32 GTest binaries
- `examples/` — simple API usage examples
- `apps/` — complex demo applications that exercise multiple HPActor subsystems
- `tools/toml-compiler/` — AOT TOML-to-binary compiler
- `third_party/` — vendored dependencies (llhttp, toml++)
- `cmake/` — CMake modules (protobuf codegen, toml++ interface target)
- `docs/superpowers/tutorials/actor-framework-tutorial.md` — usage guide
- `CLAUDE_MEMORY.md` — current project memory summary for this checkout
