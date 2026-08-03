# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Session Warmup

At the start of a substantive task, read these files first:

1. `AGENTS.md` — Codex-facing working instructions for this repo.
2. `CLAUDE.md` — Claude-facing instructions; keep shared build and architecture guidance in sync when it changes.
3. `CLAUDE_MEMORY.md` — project memory summary with current feature status, implementation history, docs, and recent test counts.
4. `.claude/rules/` — authoritative behavioral rules (one file per rule): worktree isolation, TDDFlow, architecture principles, implementation constraints, configuration, testing, and build verification.

Treat `CLAUDE_MEMORY.md` as the high-level project memory source in this checkout. If persistent memory directories are introduced later, add their exact path here instead of relying on wildcard paths.

## Project Rules

Behavioral rules for all Claude Code sessions in this repo are defined in
`.claude/rules/`. That directory is the single source of truth — each rule lives
in its own file:

| File | Rule |
|------|------|
| `01-worktree-isolation.md` | Worktree isolation, branch naming, CWD verification |
| `02-tddflow.md` | RED → GREEN → REFACTOR cycle |
| `03-architecture.md` | Actor boundaries, reliability planes, design flow |
| `04-implementation.md` | C++20 constraints, actor contracts, memory, concurrency |
| `05-configuration.md` | Subsystem-owned TOML parsers, opaque interfaces |
| `06-testing.md` | Determinism, resource isolation, meaningful tests |
| `07-build-verification.md` | Narrowest verification first, build commands |

The sections below contain project-specific reference information not covered by `.claude/rules/`.

## Build Commands

```bash
# Configure and build
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build

# Run tests
ctest --output-on-failure --parallel 8

# Run a single test
./build/tests/unit/core/test_unit_core

# Run a single GTest suite or test via filter
./build/tests/unit/core/test_unit_core --gtest_list_tests
./build/tests/unit/core/test_unit_core --gtest_filter="*ActorId*"

# Run a specific GTest case through ctest
ctest -R "ActorIdDefaultConstruction" --output-on-failure

# Build with sanitizers
cmake -DENABLE_TSAN=ON ..  # ThreadSanitizer
cmake -DENABLE_ASAN=ON ..  # AddressSanitizer

# Build options
cmake -DENABLE_EXAMPLES=OFF ..    # Disable examples (default ON)
cmake -DENABLE_APPS=OFF ..        # Disable complex demo applications (default ON)
cmake -DENABLE_PROACTOR=ON ..     # Enable proactor backend
cmake -DENABLE_MEMORY_TRACKING=OFF .. # Disable per-actor memory tracking (default ON)
cmake -DENABLE_MEMORY_DEBUG=ON .. # Enable memory poisoning + canary verification
cmake -DENABLE_ACTOR_METRICS=OFF .. # Disable actor-level metrics (default ON)
cmake -DENABLE_ACTOR_LOGGING=OFF .. # Disable structured actor logging (default ON)
cmake -DENABLE_ACTOR_TRACING=OFF .. # Disable distributed tracing (default ON)
cmake -DENABLE_CLI=OFF ..       # Disable interactive CLI subsystem (default ON, runtime opt-in via cli.enabled)
cmake -DENABLE_COVERAGE=ON ..   # Enable gcov/llvm-cov style coverage instrumentation
cmake -DENABLE_CLANG_TIDY=ON .. # Enable clang-tidy checks during C++ builds (default OFF)
```

## Architecture

HPActor is a C++20 event-based actor framework inspired by CAF (C++ Actor Framework).

### Actor Concurrency Rules

Before designing or implementing features that touch actor delivery, mailboxes,
lock-free queues, scheduler state, worker placement, timers, or actor
multi-threading, read
`docs/architecture/mailbox/actor-concurrency-and-lockfree-mailbox-rules.md`.
Treat it as the normative rule set for MPSC mailbox use, actor state ownership,
ready-gate transitions, implementation contracts, and concurrency test design.

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
| `MPSCActorMailbox` | `mailbox/mpsc_actor_mailbox.hpp` | Lock-free MPSC envelope with edge-triggered CAS wakeup, `try_push_batch()` for batch enqueue |
| `MultiLaneQueue` | `mailbox/multi_lane_queue.hpp` | Lock-free multi-lane queue with dedicated system lane, priority-aware routing |
| `ObjectPool<T,N>` | `mem/object_pool.hpp` | Lock-free bounded object pool with prefill/acquire/release for per-actor reuse |
| `StreamBuffer` | `adt/stream_buffer.hpp` | Contiguous byte buffer with `from_data()` exact-capacity factory for small payloads |
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
| `FaultController` / `FaultPoint` | `fault/*.hpp` | Deterministic fault injection with 80 sites across 14 domains, seed-replayable schedules |
| `IServiceDiscovery` | `net/service_discovery.hpp` | Pluggable discovery interface (UdpRegistrar, Gossip, Static, Hybrid) |
| `GossipMembership` | `net/gossip_membership.hpp` | SWIM protocol for decentralized cross-server discovery |
| `ActorLocationCache` | `net/actor_location_cache.hpp` | TTL cache for ActorId → EndPoint resolution |

### Header Placement Rules

New public headers must be placed according to architecture module boundaries:

| Concern | Directory |
|---------|-----------|
| Timer types (timing wheel, calendar queue) | `include/hpactor/timer/` |
| Coroutine infrastructure (tasks, awaiters, frame pool) | `include/hpactor/coroutine/` |
| Lifecycle, shutdown, passivation, drain, quarantine, circuit breaker | `include/hpactor/actor/lifecycle/` |
| Mailbox admission, backpressure, delivery, pressure monitoring | `include/hpactor/mailbox/` |
| Durable actor state | `include/hpactor/actor/durable/` |
| Scheduler internals (scheduler, workers, queues, EDF, A2WS, dispatch) | `include/hpactor/sched/` |

New source files follow the same directory structure under `src/`.

### Production Architecture Backlog

Current implemented foundations include scheduled messages, delivery-mode
configuration, receiver deduplication, structured failure envelopes, bounded
mailboxes, multi-lane priority queues, DLQ with CLI replay/export, distributed
tracing, HTTP gateway, deterministic fault injection, graceful shutdown, actor
lifecycle, and actor quarantine. Durable outbox/inbox, ACK/NACK retry, cluster
control, security, and operations-plane admin APIs remain design/backlog.

Key files:

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
- `context()->schedule(delay, msg)` — schedule self-delivery after delay (returns `AlarmHandle`)
- `context()->cancel_schedule(handle)` — cancel a pending scheduled message
- `become(Behavior)` — change behavior dynamically
- `co_await mailbox_awaiter` — suspend until message arrives (coroutine actors)

Proto actors register handlers declaratively:
- `on<T>(handler)` — fire-and-forget proto handler
- `on_request<ReqT, ResT>(handler)` — request-response proto handler

**Performance optimizations (opt-in):**
- `ActorSystem::try_deliver_local_fast(target, msg)` — enqueue directly to mailbox, bypassing DeliveryPipeline (circuit breaker, TTL, dedup, backpressure). Use when those checks are known unnecessary.
- `EventBasedActor::add_fast_tag(tag)` — register a TypeTag for fast-path dispatch. Messages with registered fast tags skip drain/lifecycle/CLI gates in `receive()` and go directly to `dispatch_user_message()`.
- `MPSCActorMailbox::try_push_batch(begin, end, meta)` — batch-enqueue N messages with a single reservation check and edge-triggered CAS wakeup. Use when multiple messages share the same target mailbox and metadata.

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

- `include/hpactor/` — public headers (actor, cli, config, core, fault, mailbox, metrics, mem, net, ref, rpc, sched, spawn, supervision, types)
- `src/` — implementation files (linked into hpactor_lib)
- `tests/` — 230+ test source files in three-tier structure (unit, integration, system) using Google Test; 32 GTest binaries are discovered through CTest (2105+ GTest cases)
- `apps/bench_saturate/` — actor system saturation benchmark (presets: quick-saturate, deep-saturate, alloc-stress, mixed-load, fan-in-extreme, fan-out-burst)
- `examples/` — simple API usage examples
- `apps/` — complex demo applications that exercise multiple HPActor subsystems
- `tools/toml-compiler/` — AOT TOML-to-binary compiler
- `third_party/` — vendored dependencies (googletest v1.14.0, llhttp, toml++)
- `cmake/` — CMake modules (gtest, protobuf codegen, toml++ interface target)
- `docs/architecture/production/` — production reliability plane, missing design docs, and refined requirement backlog
- `docs/superpowers/tutorials/actor-framework-tutorial.md` — usage guide
- `docs/superpowers/specs/2026-06-19-bench-saturate-perf-optimize-design.md` — bench saturate Round 1 design (5 optimizations)
- `docs/superpowers/specs/2026-06-19-bench-saturate-perf-optimize-round2-design.md` — Round 2 design (fast-tag receive, object pool)
- `docs/superpowers/specs/2026-06-19-bench-saturate-perf-optimize-round3-design.md` — Round 3 design (batch enqueue, deferred items)
- `.claude/projects/*/memory/` — persistent project memory
- `.claude/rules` — authoritative behavioral rules for Claude Code sessions
