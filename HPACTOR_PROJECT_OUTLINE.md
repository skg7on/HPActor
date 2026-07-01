# HPActor Project Outline for Codex

HPActor is a high-performance, production-oriented C++20 Actor framework inspired by CAF (C++ Actor Framework). It combines work-stealing schedulers, EDF real-time scheduling, multi-priority queues, and a custom two-tier slab memory allocator to achieve deterministic response times without GC pauses. The project targets million-level concurrency for distributed actor workloads.

## Quick Navigation

| What | Where |
|------|-------|
| Agent instructions | [AGENTS.md](AGENTS.md) |
| Project memory (detailed) | [CLAUDE_MEMORY.md](CLAUDE_MEMORY.md) |
| Build/architecture (Claude) | [CLAUDE.md](CLAUDE.md) |
| Production reliability roadmap | [docs/architecture/production/production-reliability-plane.md](docs/architecture/production/production-reliability-plane.md) |
| Feature gap backlog | [docs/architecture/production/feature-gap-refined-requirement-backlog.md](docs/architecture/production/feature-gap-refined-requirement-backlog.md) |
| Actor concurrency and lock-free mailbox rules | [docs/architecture/mailbox/actor-concurrency-and-lockfree-mailbox-rules.md](docs/architecture/mailbox/actor-concurrency-and-lockfree-mailbox-rules.md) |
| Architecture docs root | [docs/architecture/](docs/architecture/) |
| Tutorial | [docs/superpowers/tutorials/actor-framework-tutorial.md](docs/superpowers/tutorials/actor-framework-tutorial.md) |

## Project State

- **Tests**: 1411 source-level GTest cases across 219 test files and 32 GTest binaries
- **Test tiers**: `tests/unit/`, `tests/integration/`, `tests/system/`
- **Core runtime**: Complete (spawn, messaging, scheduling, networking, memory, config)
- **Production features**: Failure envelopes, delivery semantics, multi-lane priority mailbox, DLQ, quarantine/circuit-breaker, deterministic fault injection (80 sites, 14 domains), distributed tracing, actor metrics, structured logging, graceful shutdown, SWIM gossip membership
- **Next major area**: Production reliability data/control/operations planes (see backlog)

## Directory Layout

```
HPActor/
├── include/hpactor/      # Public headers (header-only actor-facing APIs)
│   ├── actor/            # Actor base classes, behaviors, lifecycle
│   ├── adt/              # Shared ADTs (Id<Tag,T>, ChaseLev deque, MpscRingBuffer, ...)
│   ├── cli/              # Interactive CLI types
│   ├── config/           # TOML topology config types
│   ├── core/             # ActorSystem, actor_registry, mailbox types
│   ├── fault/            # Deterministic fault injection types
│   ├── log/              # Structured logging types
│   ├── mailbox/          # MultiLaneQueue, DLQ, overflow handlers, backpressure
│   ├── mem/              # Memory management (slab allocator, region accounting, hibernation)
│   ├── metrics/          # Actor metrics types
│   ├── net/              # Networking (event loop, transport, discovery)
│   ├── ref/              # Actor references (ActorRef, ActorProxy)
│   ├── rpc/              # RPC channel
│   ├── sched/            # Scheduling subsystem
│   ├── supervision/      # Supervision strategies
│   ├── tracing/          # Distributed tracing (W3C TraceContext)
│   └── types/            # Core types, failure_reason, failure_envelope
├── src/                  # Compiled runtime (hpactor_lib)
│   ├── actor/            # actor_system.cpp, spawn_receiver, etc.
│   ├── cli/              # CLI implementations
│   ├── config/parsers/   # TOML subsystem parsers (self-registering)
│   ├── core/             # serialization.cpp
│   ├── fault/            # Fault controller, point registry, schedule
│   ├── log/              # Log manager, drain, sinks
│   ├── mailbox/          # DLQ, overflow handlers, backpressure serialization
│   ├── mem/              # Slab cache, segment provider, compaction, zram
│   ├── metrics/          # Metrics registry, aggregator, formatter
│   ├── net/              # Event loop, transport, connection pool, registrar, gossip
│   ├── ref/              # ActorRef, ActorProxy implementations
│   ├── sched/            # Scheduler, worker thread, execution engine
│   ├── tracing/          # Trace manager, exporters
│   └── ...               # Other flat files (spawn.cpp, actor_type_registry.cpp, etc.)
├── tests/
│   ├── unit/             # Narrow unit tests, organized by subsystem
│   ├── integration/      # Cross-subsystem behavior tests
│   ├── system/           # Full-system behavior and app-level tests
│   └── support/          # Test helpers, mocks, fixtures
├── apps/                 # Complex demo applications
│   ├── edgeops_telemetry/ # EdgeOps telemetry platform demo
│   └── order_platform/   # Order processing demo
├── examples/             # Simple API usage examples
├── protos/hpactor/       # Protobuf definitions (TypedMessage, frame, gossip, etc.)
├── tools/toml-compiler/  # AOT TOML-to-binary compiler
├── third_party/          # Vendored deps (googletest, llhttp, toml++, linenoise, relacy)
├── cmake/                # CMake modules
├── docs/
│   ├── architecture/     # Architecture design docs
│   │   ├── core/         # Core actor, config, link/monitor
│   │   ├── actor/        # Metrics, logging, CLI, tracing, mailbox
│   │   ├── sched/        # Scheduling design docs
│   │   ├── memory/       # Memory management design
│   │   ├── net/          # Network, service discovery
│   │   ├── ai/           # MLX accelerator integration designs
│   │   └── production/   # Production reliability plane (data/control/operations)
│   ├── app/              # App-level design docs (EdgeOps, etc.)
│   └── superpowers/      # Implementation specs and plans
│       ├── specs/        # Detailed design specs
│       ├── plans/        # Implementation plans
│       └── tutorials/    # Usage guides
└── .worktrees/           # Git worktrees for task isolation (gitignored)
```

## Actor Type Hierarchy

```
AbstractActor (interface base)
    └── LocalActor (has ActorContext)
            ├── EventBasedActor (cooperative, behavior-based, proto handlers)
            │       ├── StatefulActor<T> (explicit state)
            │       ├── ProtoStatefulActor<T> (protobuf + state)
            │       ├── SpawnReceiver (remote spawn)
            │       └── DenseComputingActor (dedicated pool dispatch)
            ├── TypedEventBasedActor<Signatures...> (statically typed)
            ├── BlockingActor (thread-based, blocking receive)
            │       └── ScopedActor (main/non-actor context)
            └── DaemonActor (dedicated thread loop)
                    ├── PollingActor (CPU affinity, poll budget)
                    ├── ExternalMsgGatewayActor (named route table)
                    │       └── net::HTTPGatewayActor (HTTP ingress)
                    └── cli::CliActor (stdin/socket I/O, command tree)

ActorRef = variant<LocalActor*, ActorProxy>  # unified reference
```

## Key Subsystems

### Core Actor (actor/, core/)
Event-based actors with `become()`, `on<T>()`/`on_request<Req,Res>()` handler registration, `co_await` coroutine message receive, link/monitor death propagation, drain/shutdown lifecycle. Messages are protobuf `TypedMessage` (TypeTag + payload).

### Scheduling (sched/)
- **HybridScheduler**: Work-stealing with A2WS adaptive victim selection
- **ChaseLevDeque**: Lock-free LIFO owner / FIFO thief work items
- **MultiPriorityWorkQueue**: Per-priority ChaseLev arrays, starvation-free
- **EDFQueue**: Earliest Deadline First min-heap, O(log n)
- **TimingWheel**: Hierarchical O(1) timer cascading (4 levels)
- **ActorExecutionEngine**: Extracted coroutine execution engine
- **WorkerThread**: Wraps std::thread, handles thread-local allocator setup

### Mailbox (mailbox/)
- **MultiLaneQueue<T>**: Lock-free multi-lane queue (dedicated system lane, priority-aware routing)
- **Overflow handlers**: RejectNewest, DropNewest, DropOldest, DropLowestPriority, SpillToOverflow, DeadLetter, SignalOnly
- **Backpressure**: PressureStateMachine, ReservationManager, BackpressureSignalGate
- **Delivery semantics**: DeliveryMode (best-effort through durable-at-least-once), DedupCache
- **Dead-Letter Queue**: Bounded record capture, snapshot API, CLI replay/export

### Memory Management (mem/)
- **Two-tier slab**: mmap-backed SegmentProvider (Tier 0) -> per-thread SlabCache (Tier 1), 8 size classes (32B-4KB)
- **Typed regions**: kActor, kMessage, kCoroutine, kNetwork, kInternal, kHibernate with per-region limits
- **Thread-local hot path**: Bump pointer <25ns, CAS freelist recycle <32ns
- **Poisoning/canaries**: 0xAA fill + 8B canary footer, guard pages
- **Hibernation**: Serialize + madvise(MADV_PAGEOUT) to ZRAM
- **Compaction**: Generation-based slab tracking, 5% fragmentation budget

### Observability (metrics/, log/, tracing/)
- **Actor metrics**: Lock-free MpscRingBuffer, OpenMetrics /metrics endpoint, configurable per-actor labels
- **Structured logging**: MPSC LogRingBuffer, pluggable sinks (stderr, file, rotating file), JSON/text formatters
- **Distributed tracing**: W3C TraceContext, parent-based sampling, OTLP/JSON exporters, protobuf propagation

### TOML Config (config/)
Declarative actor topology bootstrapping: `ActorSystem::load_topology("config.toml")`. Pipeline: TomlParser (imports, templates, validation, topological sort) -> TopologyModel -> BootstrapEngine (actor spawn in DAG order) -> SystemInit broadcast. Subsystem parsers self-register via file-scope static registrars. AOT compiler produces mmap-friendly binary format.

### Service Discovery (net/)
Pluggable `IServiceDiscovery` with 4 backends:
- **UdpRegistrar**: Same-host actor registration via async UDP
- **GossipMembership**: SWIM protocol for cross-server membership (ping/ack/PingReq, suspicion/death FSM)
- **HybridDiscovery**: Composes both
- **StaticDiscovery**: Fixed/hardcoded topology
- **ActorLocationCache**: TTL cache for ActorId -> EndPoint resolution

### Fault Injection (fault/)
Deterministic seed-replayable fault injection across 14 domains (80 sites). `FAULT_INJECT(path)` macro with `HPACTOR_UNLIKELY`. Actions: Fail, Drop, Delay, Corrupt, Panic. CLI /fault commands. Zero overhead with `ENABLE_FAULT_INJECTION=OFF`.

### CLI (cli/)
Interactive trie-based command tree: /inspect, /kill, /list, /stats, /dlq, /failure, /fault, /help, /quit. Pretty, JSON, and tabular output formatters. Pager support.

### Supervision (supervision/)
OneForOne (single child restart), AllForOne (siblings restart), SelfSupervisingActor (max restarts + interval policy).

## Key Design Constraints

- C++20, no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`)
  - Exception support enabled ONLY for `src/config/toml_parser.cpp`, `src/config/toml_table_view.cpp`, `tools/toml-compiler/compiler.cpp` (toml++ requirement)
- Header-only actor-facing types + compiled runtime library (`hpactor_lib`)
- LLVM coding standards
- Avoid `dynamic_cast`, `typeid`, exception-based control flow
- Keep blocking I/O out of event-loop/cooperative scheduler paths
- Bounded capacity + explicit failure paths over unbounded growth
- Thread-local memory allocation on hot paths; no general-purpose malloc in actor/message/coroutine paths
- Protobuf TypedMessage type tags for dynamic messages, typed actor signatures for static
- No shared mutable state between actors
- Subsystem-owned extension points over central switches (self-registering TOML parsers, `IMailbox<T>` swap-in)

## Build System

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
ctest --output-on-failure --parallel 8
```

### Key CMake Options

| Option | Default | Purpose |
|--------|---------|---------|
| `ENABLE_EXAMPLES` | ON | Build examples |
| `ENABLE_APPS` | ON | Build complex demo apps |
| `ENABLE_PROACTOR` | OFF | Proactor backend (IoUring, GCD) |
| `ENABLE_ACTOR_METRICS` | ON | Actor-level metrics |
| `ENABLE_ACTOR_LOGGING` | ON | Structured logging |
| `ENABLE_ACTOR_TRACING` | ON | Distributed tracing |
| `ENABLE_CLI` | ON | Interactive CLI (runtime opt-in) |
| `ENABLE_CLANG_TIDY` | OFF | Clang-tidy checks during build |
| `ENABLE_COVERAGE` | OFF | Code coverage instrumentation |
| `ENABLE_MEMORY_DEBUG` | OFF | Poisoning + canary verification |
| `ENABLE_FAULT_INJECTION` | ON | Deterministic fault injection hooks |
| `ENABLE_TSAN` / `ENABLE_ASAN` | OFF | Sanitizers |

## Development Workflow (Required by AGENTS.md)

### Git Worktree Isolation
Every design or implementation job must happen in an isolated git worktree under `.worktrees/<short-task-name>` on a task-specific branch.

1. Branch from main: `git checkout -b codex/my-task`
2. Create worktree: `git worktree add .worktrees/my-task codex/my-task`
3. Do all edits in the worktree
4. The `.worktrees/` directory is gitignored
5. Pure read-only inspection may stay in the main checkout

### Build Verification Discipline
After code modifications, prefer the narrowest verification:
- Targeted `ninja <target>` for a single library/binary
- One test binary: `./build/tests/<tier>/<subsystem>/test_<name>`
- Filtered ctest: `ctest -R <pattern> --output-on-failure`
- Full configure/build/test cycle only for cross-cutting changes (build config, generated files, public headers, broad runtime behavior)

### Before Starting Work
Read in order:
1. `AGENTS.md` -- Codex-facing instructions
2. `CLAUDE.md` -- Shared build/architecture guidance
3. `CLAUDE_MEMORY.md` -- Project memory, current feature status, test counts

## Testing Strategy

Three tiers matching risk profile:

| Tier | Scope | Location |
|------|-------|----------|
| Unit | Local behavior, single class/function | `tests/unit/<subsystem>/` |
| Integration | Cross-subsystem interactions | `tests/integration/<area>/` |
| System | Full-stack behavior, app workflows | `tests/system/` |

- 1411 source-level `TEST`/`TEST_F`/`TEST_P` definitions across 219 files and 32 GTest binaries
- Google Test v1.14.0 vendored at `third_party/googletest/`
- `gtest_discover_tests` used for individual case registration with ctest
- Sanitizer, fault injection, and stress tests for reliability-plane features

## Key Files & Entry Points

- `include/hpactor/core/actor_system.hpp` -- Main system entry point
- `include/hpactor/actor/actor_context.hpp` -- Actor execution context (`send()`, `reply()`, `become()`)
- `include/hpactor/behavior.hpp` -- Message handler pattern matching
- `include/hpactor/actor/typed_actor.hpp` -- Statically typed actor with `on<T>()` handlers
- `include/hpactor/mailbox/multi_lane_queue.hpp` -- Core mailbox data structure
- `include/hpactor/sched/hybrid_scheduler.hpp` -- Primary scheduler
- `include/hpactor/net/event_loop.hpp` -- kqueue/epoll event loop
- `include/hpactor/config/toml_parser.hpp` -- TOML topology bootstrap
- `include/hpactor/mem/slab_cache.hpp` -- Thread-local slab allocator
- `include/hpactor/metrics/metrics_actor.hpp` -- OpenMetrics exposition
- `include/hpactor/tracing/trace_context.hpp` -- W3C TraceContext
- `include/hpactor/fault/fault_controller.hpp` -- Fault injection controller
- `include/hpactor/types/failure_reason.hpp` -- Canonical failure reasons (23 values)
- `include/hpactor/types/failure_envelope.hpp` -- Structured failure metadata
- `include/hpactor/supervision/one_for_one.hpp` -- Supervision strategy
- `src/net/gossip_membership.cpp` -- SWIM gossip protocol

## Notes for Codex

- The project uses GoogleTest v1.14.0 (vendored); tests use `TEST`/`TEST_F`/`TEST_P` macros
- GTest discover tests finds individual test cases; use `ctest -R <pattern>` or direct binary `--gtest_filter`
- Protobuf types live in `protos/hpactor/`; regenerate with build system
- TOML subsystem parsers self-register via file-scope static objects in `src/config/parsers/`
- New subsystem config requires a parser in `src/config/parsers/`, not edits to `parse_file_data`
- All new files need Apache 2.0 license headers per project convention
- No `dynamic_cast`, `typeid`, or exception-based flow outside the 3 exempted files
- Lock-free, scheduler, mailbox, timer, or transport changes need concurrency contract documentation and stress/race tests
- Protocol, binary topology, or persisted-state changes need backward-compatibility checks
