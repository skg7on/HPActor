# HPActor

<picture>
  <source srcset="docs/project-logo/assets/hpactor-logo.png" media="(prefers-color-scheme: dark)">
  <img src="docs/project-logo/assets/hpactor-logo-preview.png" alt="HPActor logo" width="600">
</picture>

[![CI](https://github.com/skg7on/HPActor/actions/workflows/ci.yml/badge.svg)](https://github.com/skg7on/HPActor/actions/workflows/ci.yml)
[![Coverage](https://skg7on.github.io/HPActor/coverage-badge.svg)](https://skg7on.github.io/HPActor/coverage-html/)

A high-performance distributed Actor framework with million-level concurrency support. Combines work-stealing schedulers, EDF (Earliest Deadline First) real-time scheduling, multi-priority queues, and an application-defined two-tier slab memory allocator for deterministic response times without GC pauses.

## Documentation

The **[HPActor Developer Manual](docs/manual/index.rst)** is the authoritative guide for building, deploying, monitoring, and operating HPActor-based systems. It is written in reStructuredText and built with Sphinx.

| Section | Description |
|---------|-------------|
| [Getting Started](docs/manual/getting-started/overview.rst) | Framework overview, installation, your first actor, project structure |
| [Building Applications](docs/manual/building-applications/actor-types.rst) | Actor types, message passing, lifecycle, topology config, remote actors, distributed patterns |
| [Monitoring](docs/manual/monitoring/metrics.rst) | Prometheus metrics, structured logging, distributed tracing, health/readiness |
| [Operations](docs/manual/operations/cli.rst) | Interactive CLI, CLI server, HTTP gateway, daemon mode with systemd |
| [SRE Integration](docs/manual/sre-integration/prometheus-grafana.rst) | Prometheus + Grafana, Loki, Jaeger, AlertManager, chaos engineering |
| [Best Practices](docs/manual/best-practices/actor-design.rst) | Actor design, error handling, performance tuning, testing, deployment |
| [Limitations](docs/manual/limitations.rst) | Known gaps, scalability limits, platform support, roadmap |

To build the manual locally:

```bash
cd docs/manual
pip install sphinx sphinx-rtd-theme
make html
```

Built HTML lands in `docs/manual/_build/html/`. In the future, the manual will be published to the project's GitHub Pages alongside the API coverage report.

## Recent Work (June 7–14, 2026)

This week delivered the daemon service architecture for Linux systemd deployment, AI accelerator subsystem foundations, scheduler reliability hardening, mailbox thread-safety formal validation, reliable messaging primitives, and a performance benchmark app — ~236 commits across 274 files (+39,742 −2,558).

### Daemon Service & CLI Decoupling (sys-284)
- **ProcessManager**: Singleton orchestrating daemonization (double-fork, setsid, umask, PID file), systemd `sd_notify` protocol (READY/STOPPING/STATUS/WATCHDOG), signal handling via signalfd (Linux) / self-pipe (macOS), and foreground/daemon mode dispatch. All thread creation happens after daemonization to preserve inherited file descriptors.
- **CliServerActor**: Socket-based CLI server using `TcpAcceptor` + `UnixDomainAcceptor` + EventLoop for non-blocking I/O, replacing stdin-based CLI in daemon mode. Each connection spawns an independent `CliSession`.
- **CliSession**: Transport-agnostic command processor extracted from `CliActor` — parses input, routes through the trie-based `CommandNode` tree, and writes formatted output. Shared between stdin-based `CliActor` and socket-based `CliServerActor`.
- **hpactor-cli**: Standalone CLI client binary (`tools/hpactor-cli/`) connecting via UDS or TCP with readline-style line editing, ANSI color output, `/attach` and `/detach` session management.
- **HealthHttpServer**: Non-blocking HTTP health endpoint reusing `HTTPConnection` and EventLoop, delegated to `HTTPGateway` for route registration. Returns 200/503 with JSON body.
- **WatchdogActor**: Periodic `sd_notify("WATCHDOG=1")` heartbeats at half the systemd `WatchdogSec` interval. Detects scheduler stalls via `HealthHttpServer` response latency.
- **SyslogSink**: POSIX `syslog(3)` log sink implementing `ILogSink`, with facility and level mapping. Wired into `LogManager` for daemon-mode logging.
- **systemd unit**: `deploy/systemd/hpactor.service` — Type=notify, WatchdogSec=10s, security hardening (ProtectSystem=strict, NoNewPrivileges, etc.), and production `deploy/systemd/config.toml`.
- **Self-registering TOML parser**: `[system.process]` with mode (foreground/daemon), PID file, UDS path, TCP port, watchdog interval, and health endpoint config.
- **Test coverage**: 7 new test files — `test_process_manager` (18 tests), `test_cli_session` (4), `test_cli_server_actor` (2), `test_watchdog_actor` (1), `test_health_http` (2), `test_syslog_sink` (4), `test_daemon_integration` (4).

### AI Accelerator Subsystem (AI-ACC-001A, AI-ACC-001B)
- **Accelerator type system** (`include/hpactor/ai/`): `AcceleratorType` enum (CPU, GPU, NPU, FPGA, DSP, Custom), `AcceleratorCapability` bitmask, `AcceleratorConfig` with typed device descriptors (CUDA, ROCm, OneAPI, OpenCL, Apple Silicon, Qualcomm AI, ARM NN, custom), `NodeResourceSummary` for cluster-wide resource reporting, and `AiTypeTags` for protobuf message routing.
- **Protobuf schema** (`protos/hpactor/ai_resource.proto`): `PbAcceleratorConfig`, `PbNodeResourceSummary`, `PbAcceleratorCapability` messages for wire-level resource negotiation.
- **TOML config parser**: Self-registering `AiAcceleratorConfigParser` in `src/config/parsers/` — parses `[ai.accelerators]` array-of-tables with per-device type, device ID, memory budget, TTL, thread affinity, and mock device flag. 6 TOML fixture files for validation (valid CPU, disabled, duplicate ID, invalid enum, invalid TTL, mock).
- **Build gate**: `ENABLE_AI_ACCELERATORS` CMake option — gates all AI headers, sources, protobuf codegen, and tests behind a single compile-time switch.
- **Config → SystemDef wiring**: `AcceleratorConfig` flows from TOML parser through `TopologyModel::SystemDef` into `ActorSystem::Config` for runtime access.
- **Test coverage**: `test_accelerator_types` (513 lines), `test_ai_accelerator_config` (134 lines).

### Scheduler Reliability Hardening
- **Lost-wakeup window closed on x86_64** (PR #289, #290): Replaced yield-based polling with adaptive condition-variable wait on Linux (`futex`), eliminating the lost-wakeup race where a CAS transition between gate checks left workers permanently idle. Platform-specific backoff: `futex(WAIT)` on Linux, `std::this_thread::yield()` on macOS (no futex).
- **Rate-limiter spin loop capping** (PR #293): Capped lost-wakeup re-admissions to prevent infinite spin when the mailbox repeatedly drains and re-fills under high throughput, bounding CPU waste while preserving sub-µs wakeup latency.
- **Adaptive worker idle model** (PR #291): Workers now track their idle mechanism (polling vs. CV wait) and expose it via `WorkerThread::idle_model()`. CLI `/scheduler workers` displays per-worker idle model for observability.
- **Thread ID display fix** (PRs #297, #298): Replaced `std::hash<std::thread::id>` with `native_handle()` and `syscall(SYS_gettid)` on Linux to display real kernel TIDs in scheduler and actor diagnostics, matching `ps`/`htop` output.
- **TimingWheel hardening**: Added missing edge cases for cascading timer wheel advancement, preventing timer drift under high-frequency short-interval schedules.
- **Test coverage**: `test_lost_wakeup_rate_limit` (176 lines), expanded `test_worker_thread` and `test_timing_wheel`.

### Mailbox Thread Safety & Formal Validation
- **Prearm race detection & fix** (PR #286): Identified and closed a prearm race in `MPSCActorMailbox` where an enqueue between the empty-check and the prearm CAS caused the mailbox to never transition to ready. Added `test_prearm_race` (378 lines) with adversarial interleaving.
- **Formal validation test suite**: `test_mailbox_formal_validation` (366 lines) and `test_mailbox_stress_formal` (384 lines) — systematic interleaving coverage for MPSC correctness properties (no lost messages, no double-delivery, FIFO ordering within producer).
- **MPSCMailbox ARM64 concurrency history**: Added detailed Doxygen documenting the ARM64 weak-memory-order revision, the CAS prearm protocol, and the ABA-safe incarnation counter.
- **WorkerThread adaptive idle documentation**: Full Doxygen on `WorkerThread` with idle model semantics, futex polling path, and CV signaling contract.

### Reliable Messaging Foundation (MSG-004)
- **ACK/NACK frame types**: `PbAckFrame` and `PbNackFrame` protobuf messages in `frame.proto`, with TypeTag assignments and serialization dispatch in `ProtoTypeRegistry`.
- **Retry policy**: `RetryPolicy` with exponential backoff, jitter, max retries, and deadline-based abandonment. Configurable per-message and per-actor defaults.
- **Outbound delivery tracker**: `OutboundDeliveryTracker` — tracks in-flight messages by `MessageId`, correlates ACK/NACK responses, triggers retry on timeout or NACK, and emits `DeliveryReceipt` on terminal resolution (delivered, abandoned, expired).
- **Delivery receipt**: `DeliveryReceipt` — terminal delivery proof with final status, attempt count, elapsed time, and failure reason. Routed back to sender actor via system message.
- **Delivery pipeline**: `DeliveryPipeline` — composes serialization → admission → enqueue into a single coordinated path, replacing ad-hoc delivery logic in `try_deliver_local()`.
- **Durable delivery store interface**: `IDurableDeliveryStore` abstract interface for outbox/inbox durability (implementation backlog).
- **CLI commands**: `/reliable status [actor_id]`, `/reliable retry <msg_id>`, `/reliable abandon <msg_id>` registered in `src/cli/commands/reliable_commands.cpp`.
- **Test coverage**: `test_ack_nack_frames`, `test_retry_policy`, `test_outbound_delivery_tracker`, `test_delivery_receipt`.

### Performance Benchmark App
- **`apps/bench_perf/`**: New benchmark application with 4 actor types — `BenchCoordinatorActor` (orchestrates benchmark runs), `BenchWorkerActor` (message echo/ping-pong throughput), `BenchCollectorActor` (latency histogram aggregation), `BenchHotActor` (sustained high-throughput spam). Configurable message size, actor count, and duration via CLI `/bench` commands.
- **Integrated CLI**: `/bench run <scenario>`, `/bench stop`, `/bench results`, `/bench history` — on-demand benchmarks without restarting the actor system.
- **Smoke test**: `test_bench_perf_smoke` validates benchmark app initialization, message flow, and result collection.

### CLI Demo Enhancements
- **New CLI commands**: `/scheduler workers` (per-worker idle model, thread ID, queue depth), `/tracing spans` (active span inspection), `/log level <actor_id> [level]` (dynamic log level), `/memory regions` (typed region pressure/limit stats).
- **Memory commands**: `/memory actor <id>` — detailed per-actor allocation breakdown by region with pressure state and rejection counts.
- **Command tree builder**: `CommandTreeBuilder` utility class in `include/hpactor/cli/command_tree_builder.hpp` — fluent API for registering multi-level commands with argument parsers and help text, reducing boilerplate in command registration.

### Documentation & Process
- **Branch naming convention** (PR #295): Standardized `category/short-description` kebab-case pattern (`worktree/`, `feature/`, `fix/`, `docs/`, `refactor/`) codified in `.claude/rules`.
- **CLAUDE.md / rules deduplication** (PR #295): Removed redundant rules from `CLAUDE.md`, consolidated authoritative behavioral rules in `.claude/rules`, leaving `CLAUDE.md` as reference material.
- **New skills**: `hpactor-deterministic-tests` (deterministic test patterns for schedulers, mailboxes, timers, coroutines) and `hpactor-scheduler-thread-safety` (concurrency review for scheduler, work stealing, mailbox, and ready-state changes).
- **Strict Doxygen**: `MPSCMailbox` with ARM64 concurrency history, `WorkerThread` with adaptive idle + CV documentation, `TimingWheel` cascading algorithm, `CalendarQueue` bucket semantics.

### Code Review & Quality
- **15 review findings addressed** (PR #286): Daemon service post-merge review — `HealthHttpServer` refactored to reuse `HTTPConnection` + EventLoop (removed hand-rolled HTTP), `CliServerActor` switched to `TcpAcceptor`/`UnixDomainAcceptor`, `CliSession` extracted as transport-agnostic processor, `ProcessManager::send_notify()` made portable (removed Linux-only `#ifdef`), `HealthHttpServer` delegated to `HTTPGateway` for route registration.
- **Daemon service review** (PR #290): 10 additional review issues fixed — process_manager error handling, CLI server connection lifecycle, health HTTP timeout wiring, syslog level mapping.

## Features

### Actor Model
- **Actor Type Hierarchy**: Event-based, blocking, typed, and stateful actors with unified `ActorRef` references
- **Dynamic Behavior**: Actors change message handlers at runtime via `become()`
- **Coroutine-Powered**: C++20 stackless coroutines — thousands of actors multiplexed onto a small thread pool
- **Unified Message Passing**: `TypedMessage` wraps any protobuf payload with sender address for request/response routing
- **ActorRefCache**: Lock-free LRU cache for resolved `ActorRef` lookups, O(1) amortized
- **Error Reply**: `reply_with_error()` / `reply_with_result()` for structured error handling across the network
- **Opaque Identifiers**: Shared `Id<Tag, T>` template backs `ActorId`, `MessageId`, `AlarmHandle`, and timer IDs to prevent accidental cross-domain comparisons

### Protobuf-Native Programming Model
- **proto_actor**: Base class with template handler registration — `on<T>()` for fire-and-forget, `on_request<ReqT, ResT>()` for request-response
- **ProtoStatefulActor\<T\>**: Protobuf actor with explicit state access via `state()`
- **ProtoTypeRegistry**: Maps `TypeTag` to protobuf message types with 4-byte BE TypeTag + payload wire format
- **Zero-Copy Potential**: Protobuf messages flow from wire to handler without intermediate variant wrapping

### Scheduling Subsystem
- **HybridScheduler**: Work-stealing scheduler with A2WS adaptive victim selection
- **ChaselevDeque\<T\>**: Lock-free work-stealing deque (LIFO owner pop, FIFO thief steal)
- **MultiPriorityWorkQueue**: Per-priority ChaseLev arrays — starvation-free priority scheduling
- **EDFQueue**: Earliest Deadline First min-heap for real-time work, O(log n) push/pop
- **TimingWheel**: Hierarchical O(1) timer wheel with cascading (4 levels)
- **CoroutineFramePool**: Lock-free stack pool for coroutine frames, O(1) acquire/release

### Mailbox
- **MultiLaneQueue\<T\>**: Lock-free multi-lane queue with dedicated system-message lane and priority-aware user lane routing
- **MPSCActorMailbox\<T\>**: Edge-triggered CAS wakeup — no lost wakeups, no spurious rescheduling
- **Bounded Admission**: Configurable capacity, high/low watermarks, overflow policies (RejectNewest, DropNewest, DropOldest, DropLowestPriority, DeadLetter, SpillToOverflow, and more)
- **Rate Limiting & Admission Control**: `ActorRateLimiter` with 5 admission policy implementations (size limit, type filter, sender filter, priority threshold, per-sender rate) wired into mailbox admission gate
- **Backpressure Signals**: `EnqueueResult` with pressure ratio, retry-after hint, and upstream `BackpressureMode` (local, remote, both)
- **Delivery Modes**: `DeliveryMode` declares best-effort, observable best-effort, at-least-once, and durable-at-least-once policy intent for runtime delivery paths
- **Deadlines and Deduplication**: TTL enforced at enqueue, handler receive, and coroutine awaiter; `DedupCache` suppresses duplicate `(source node, source actor, message id)` tuples for tracked delivery
- **Delivery Result API**: `DeliveryResult` unified delivery outcome type for `try_send` with metric aggregation from `EnqueueResult` codes
- **Dead-Letter Queue**: Bounded record capture with reason/source tracking, payload sampling, snapshot and pop APIs, and `/dlq` CLI commands (list, show, replay, export)
- **Swap-in Interface**: `IMailbox<T>` allows replacing the mailbox implementation without touching actor code

### Memory Management
- **Two-Tier Slab Allocator**: mmap-backed SegmentProvider (Tier 0) → per-thread SlabCache with bump+freelist (Tier 1), 8 size classes (32B–4KB)
- **Typed Memory Regions**: Separate allocation pools for actors, messages, coroutines, network buffers, internal structures, and hibernation, with per-region hard limits, high-water pressure state, and rejected-allocation counters
- **Thread-Local Hot Path**: Bump pointer allocation < 25ns, lock-free CAS freelist recycle < 32ns
- **Per-Actor Observability**: 64-byte cache-line-aligned atomic counter array indexed by ActorId, lock-free telemetry ring buffer
- **Debugging**: Memory poisoning (0xAA), canary verification on alloc/free, guard pages with SIGSEGV/SIGBUS handler
- **Hibernation**: Serialize actor state → `madvise(MADV_PAGEOUT)` to ZRAM → 3-4× effective memory capacity for idle actors
- **Compaction**: Generation-based slab tracking with 5% fragmentation budget, relocatable actors by ActorId
- **Zero malloc in hot path**: Custom allocator routes all actor/message/coroutine allocations away from general-purpose `malloc`

### Observability & Metrics
- **Actor-Level Metrics**: Native OpenMetrics exposition — mailbox depth, message processing latency, actor lifecycle counters, scheduler steals, supervision restarts — served via HTTP `/metrics` endpoint for Prometheus/Grafana
- **Failure and Containment Events**: Delivery failures, quarantines, and unquarantines feed the metrics aggregator with canonical reason labels
- **Out-of-Band Instrumentation**: Lock-free `MpscRingBuffer<T>` with CAS-based event writes — zero hot-path overhead for user actors
- **OpenMetrics Format**: `# HELP`/`# TYPE` metadata, `Counter`/`Gauge`/`Histogram` with exponential bucketing, Prometheus-compatible output
- **Per-Actor Labeling**: `actor_id` and `actor_type` labels for drill-down debugging, configurable cardinality via `per_actor_labels`
- **TOML Configurable**: `[system.metrics]` section — enable/disable, ring buffer capacity, scrape path
- **Compile-Time Disable**: `ENABLE_ACTOR_METRICS=OFF` for zero-overhead deployments

### Structured Logging
- **LogRingBuffer**: MPSC lock-free bounded ring buffer for log event capture
- **LogDrain**: Consumer thread with batching and configurable sink writes
- **ILogSink Interface**: Pluggable sinks — `StderrSink`, `FileSink`, `RotatingFileSink` (size-based rotation), `MemorySink` (testing)
- **Formatters**: `TextLogFormatter` (human-readable), `JsonLogFormatter` (machine-parseable)
- **LogManager**: Owns config, ring buffer, drain, and sinks — wired into ActorSystem lifecycle
- **Subsystem Events**: Lifecycle, config/bootstrap, mailbox depth, scheduler, memory, registrar/discovery, network
- **TOML Configurable**: `[system.logging]` section — level, sinks, rotation, format

### Distributed Tracing
- **W3C TraceContext**: 16-byte TraceId, 8-byte SpanId, TraceFlags — propagated through TypedMessage and ActorMsgFrame
- **Actor Receive Spans**: Consumer-kind spans with RAII guard on every early-return path
- **Local Propagation**: Send/reply propagation via ActorContext current trace scope
- **Remote Propagation**: PbTraceContext in frame.proto, conversion helpers in frame_protobuf.cpp
- **TraceManager**: Parent-based sampling, bounded MPSC ring buffer, drain thread
- **Exporters**: Memory (testing), JSON file, OTLP HTTP
- **TOML Configurable**: `[system.tracing]` section — enabled, service_name, exporter, sample_ratio
- **Compile-Time Disable**: `ENABLE_ACTOR_TRACING=OFF` for zero-overhead deployments

### Production Reliability Architecture
- **Reliability Plane Roadmap**: `docs/architecture/production/production-reliability-plane.md` organizes the next evolution into data plane, control plane, and operations plane work
- **Refined Requirement Backlog**: `docs/architecture/production/feature-gap-refined-requirement-backlog.md` maps subsystem gaps to architecture requirements, dependencies, acceptance evidence, observability, and tests
- **Design Docs**: delivery semantics, DLQ, cluster failure model, sharding/placement, reliable messaging, durable state, graceful shutdown/rolling upgrade, security, operations/SRE, dynamic config, and chaos/reliability testing
- **Implementation Status**: Scheduled messages, delivery-mode configuration, receiver deduplication, structured failure envelopes, bounded mailboxes, multi-lane priority queues, dead-letter queue with CLI replay/export, distributed tracing (W3C TraceContext + OTLP), HTTP gateway, graceful shutdown, actor lifecycle, actor quarantine, circuit breaker (ACT-005), rate limiting with admission policies (ACT-006), ask/timeout with AskManager (ACT-007), actor passivation (ACT-008), per-message deadline/TTL enforcement (MSG-003), and delivery result API (MSG-002) are implemented. Durable outbox/inbox, ACK/NACK retry, cluster control, security, and operations-plane admin APIs remain in design.

### Failure Semantics & Containment
- **Canonical Reasons**: `FailureReason` and `FailureSource` provide a shared failure vocabulary across actor send, mailbox admission, RPC, spawn, transport, DLQ, tracing, metrics, and CLI output
- **FailureEnvelope**: Compact stack-friendly failure carrier with actor/sender/receiver/message/trace metadata, retryability, monotonic timestamp, source, and bounded detail text
- **Failure Mappings**: `EnqueueResultCode`, `errors::`, spawn errors, and `DeadLetterReason` map back to canonical failure reasons
- **Circuit Breakers**: per-actor `CircuitBreakerTracker` transitions Closed → Open → HalfOpen based on failure-rate EMA and timeout observations, with cooldown and probe handling
- **Quarantine**: `QuarantinePolicy` can escalate repeatedly failing actors from supervision restart loops into message rejection with explicit `Quarantined` / `CircuitOpen` reasons
- **Rate Limiting**: `ActorRateLimiter` with 5 admission policies (size limit, type filter, sender filter, priority threshold, per-sender rate) rejects messages exceeding rate or policy limits
- **Ask/Timeout**: `AskManager` tracks in-flight requests with deadline-based resolution; `DeadLetterReason::AskTimeout` for expired ask deadlines
- **Passivation**: `MemoryPressureMonitor` triggers automatic passivation under memory pressure; `PassivationManager` orchestrates state serialization, route suspension, and reactivation
- **CLI Visibility**: `/failure reasons` lists reason codes and retryability; `/failure summary` reports the wired mappings and delivery-failure metric status; `/actor rate` and `/actor admission` for rate-limiting status

### Actor Lifecycle
- **ActorState**: Atomic state machine (Idle → Ready → Running → IOWaiting → Terminated) with CAS transitions
- **LifecycleActor Mixin**: Opt-in lifecycle state machine (Starting → Active → Draining → Stopping → Stopped, with Failed/Recovering/Passivating/Passivated paths) with constexpr `StateDef` transition table
- **Message Gate**: Rejects user messages outside Active state and applies drain policy during Draining
- **Hierarchical Supervision**: OneForOne (restart failed child) and AllForOne (restart all children) strategies
- **SupervisorActor**: Supervises children via strategy pattern
- **SelfSupervisingActor**: Manages own children with configurable policy (max restarts, restart interval)
- **Remote Child Tracking**: Supervision across process boundaries

### Graceful Shutdown
- **DrainPolicy**: Per-actor drain behavior (Drain, DropUserMessages, ImmediateStop, with SnapshotAndStop/TransferShard reserved)
- **ActorSystem::shutdown()**: Phase-machine coordinator — Running → DrainingIngress → DrainingActors → LeavingCluster → FlushingTelemetry → Stopped
- **ActorContext::stop() / stop_sync()**: Graceful actor termination API with drain completion
- **System Actors Drain Last**: `is_system_actor()` virtual ensures infrastructure actors outlive application actors
- **CLI Commands**: `/system drain` and `/system stop` for operational control
- **TOML Configurable**: `[system.shutdown]` section — drain_timeout, stop_timeout, phase ordering

### Networking
- **EventLoop**: kqueue (macOS) / epoll (Linux) edge-triggered event loop with timer support
- **Reactor/Proactor Separation**: `IReactorBackend` interface — `EpollBackend`/`KqueueBackend` (reactor) and `IoUringBackend`/`GcdBackend` (proactor)
- **TCP Transport**: 4-byte length-prefixed framing with `ConnectionPtr` abstraction
- **TLS 1.3**: AES-256-CBC encryption, RSA key exchange, `TlsConnection` state machine
- **Connection Pool**: Dynamic pooling per node, round-robin, exponential backoff reconnect
- **UNIX Domain Socket**: Registry-driven UDS path lookup with TCP fallback
- **Async RPC**: `RpcChannel` with at-least-once delivery, retry on timeout, `RpcFuture<bytes>`

### Service Discovery
- **Pluggable Architecture**: `IServiceDiscovery` interface with 4 backends — swap without changing actor code
- **Embedded Registrar**: UDP discovery + TCP registration with heartbeat, failover, and protobuf serialization (single-server / same-host multi-process)
- **Gossip (SWIM)**: Decentralized membership for multi-server clusters — direct + indirect ping probes, incarnation-based conflict resolution, suspicion → dead state machine, ~4.4s failure detection
- **Hybrid Mode**: Composes UdpRegistrar (same-host) + GossipMembership (cross-server) for multi-process multi-server deployments
- **Static Routes**: Fixed-topology discovery for firewalled / edge deployments
- **ActorLocationCache**: TTL cache for `ActorId` → `EndPoint` resolution, integrated into `ActorProxy::send()`
- **HostResolver**: DNS resolution with caching
- **NodeRegistry**: Registry of known nodes with static routes
- **TOML Configurable**: `[system.discovery]` section — backend selection, gossip parameters, static members

### Remote Actor Spawn
- **AsyncActor**: Non-blocking spawn handle with `get()`, `ready()`, `cancel()`
- **ActorTypeRegistry**: Register spawnable actor types by name
- **SpawnReceiver**: System actor for handling spawn requests across the network
- **Well-Known System IDs**: `SpawnReceiverId`, `SystemActorType` — constexpr initialized

### Interactive CLI
- **Hierarchical Command Tree**: Trie-based `CommandNode` registry — `/actor <id> show`, `/system stats`, `/metrics show` with tab-completion-ready traversal
- **Thread-Safe Introspection**: `InspectStateRequest`/`InspectStateReply` message pair — CLI never reads actor memory directly, target actor handles request on its own thread
- **Dedicated I/O Thread**: `CliActor` extends `DaemonActor` with its own OS thread, blocks on stdin without disrupting compute workers
- **Pluggable Output Formats**: `PrettyFormatter` (ANSI box-drawing), `JsonFormatter` (machine-readable), `TabularFormatter` (grep/awk-friendly)
- **Interactive Paging**: Cursor-based `/actor list` with n/p/q/search/goto navigation, 50 actors per page
- **Virtual `to_metadata()` Interface**: Every actor exposes lightweight inspectable summary — no CLI knowledge of specific actor types needed
- **Actor Management Commands**: `/actor rate` and `/actor admission` for rate-limiting status, `/dlq list/show/replay/export` for dead-letter queue operations, `/ask pending/stats` for in-flight request tracking
- **Remote Attach Ready**: Configurable UDS/TCP listener for `hpactor attach` from separate process (future frontend)
- **TOML Configurable**: `[system.cli]` section — enable/disable, listen path, default format, page size
- **Runtime Opt-In**: CLI actor spawned only when `cli.enabled = true` in config (default: false)

### Declarative Topology Configuration
- **TOML-Based Topology**: Declare actor trees, supervision hierarchies, and dispatcher bindings in TOML — `ActorSystem::load_topology("config.toml")` bootstraps the entire system
- **Template System**: Reusable actor templates with argument merging for DRY topology definitions
- **AOT Compiler**: `tools/toml-compiler/` compiles TOML topology to a compact binary format for production deployment
- **Binary Format**: mmap-friendly binary topology with string interning — zero-parse bootstrap

### Serialization
- **Protobuf Wire Format**: `common.proto` (endpoint types), `frame.proto` (WireFrame transport), `messages.proto` (system messages)
- **DefaultSerializer**: Protobuf-based encode/decode for all system message types
- **CommunicationEndpoint**: `std::variant<Ipv4Endpoint, Ipv6Endpoint>` — network-byte-order storage for zero-copy socket operations

## Architecture

### Actor Type Hierarchy

```
AbstractActor (interface base)
└── LocalActor (has ActorContext access)
        ├── EventBasedActor (cooperative, behavior-based, coroutine-powered)
        │       ├── StatefulActor<T> (explicit state)
        │       └── TypedEventBasedActor<Signatures...> (statically typed)
        ├── BlockingActor (thread-based, blocking receive)
        │       └── ScopedActor (for main/non-actor contexts)
        └── ProtoActor (protobuf-native, on<T>() / on_request<ReqT,ResT>())
                └── ProtoStatefulActor<T> (protobuf + explicit state)
```

### Message Flow

Actors communicate via `TypedMessage` (protobuf payload with sender address):

```cpp
context()->send(addr, msg);        // send message to actor
context()->reply(msg);             // reply to current sender
context()->reply_with_error(code); // reply with error to sender
become(Behavior);                  // change behavior dynamically
co_await mailbox_awaiter;          // suspend until message arrives
```

### Actor References

```
ActorRef (std::variant)
├── Actor        — shared_ptr to local actor (direct dispatch)
└── ActorProxy   — remote actor handle (transport-based send)
```

`ActorRef` unifies local and remote references — callers use `send()` without knowing where the actor lives. Resolution uses `ActorRefCache` for O(1) amortized lookups.

### Scheduling Subsystem

| Component | Purpose |
|-----------|---------|
| `HybridScheduler` | Work-stealing scheduler with A2WS adaptive victim selection |
| `ChaselevDeque<T>` | Lock-free work-stealing deque (LIFO owner, FIFO thief) |
| `MultiPriorityWorkQueue` | Per-priority ChaseLev arrays (0=highest) |
| `EDFQueue` | Earliest Deadline First min-heap for real-time work |
| `TimingWheel` | Hierarchical O(1) timer wheel with cascading |
| `MultiLaneQueue<T>` | Lock-free multi-lane queue with dedicated system lane |
| `MPSCActorMailbox<T>` | Edge-trigger wrapper with CAS wakeup, rate limiter + admission gates |
| `CoroutineTask` | C++20 coroutine handle wrapper for actor coroutines |
| `CoroutineFramePool` | Lock-free stack pool for coroutine frames |

### Memory Management

| Component | Purpose |
|-----------|---------|
| `SegmentProvider` | Tier 0: mmap-based segment acquisition (2MB segments), carves slabs for thread-local caches |
| `SlabCache` | Tier 1: per-size-class slab with bump allocator + lock-free CAS freelist |
| `ThreadLocalAllocator` | Per-thread allocator owning 8 SlabCaches (32B–4KB) |
| `MemoryTracker` | Per-actor shadow counters (64B-aligned atomic array, 1M actor capacity) |
| `TelemetryRingBuffer` | Lock-free MPSC ring buffer for allocation event sampling |
| `HibernationRegistry` | Concurrent map of ActorId → serialized hibernation buffers |
| `CompactionManager` | Generation-based slab tracking with 5% fragmentation budget |
| `ZramManager` | `madvise(MADV_PAGEOUT/COLD/WILLNEED)` hints for ZRAM integration |

### Network Layer

| Component | Purpose |
|-----------|---------|
| `EventLoop` | kqueue (macOS) / epoll (Linux) edge-triggered event loop |
| `IReactorBackend` | Unified backend interface for reactor and proactor modes |
| `EpollBackend` | Linux epoll reactor backend |
| `KqueueBackend` | macOS kqueue reactor backend |
| `IoUringBackend` | Linux io_uring proactor backend |
| `GcdBackend` | macOS GCD proactor backend |
| `TcpTransport` | TCP transport with TLS 1.3 support |
| `PlainConnection` | Raw TCP with 4-byte length-prefixed framing |
| `TlsConnection` | AES-256-CBC encryption, RSA key exchange |
| `ConnectionPool` | Dynamic pooling with exponential backoff |
| `IServiceDiscovery` | Pluggable discovery interface (UdpRegistrar, Gossip, Static, Hybrid) |
| `GossipMembership` | SWIM protocol for decentralized cross-server discovery |
| `ActorLocationCache` | TTL cache for ActorId → EndPoint resolution |
| `Registrar` | UDP discovery + TCP registration with heartbeat |
| `HostResolver` | DNS resolution with caching |
| `RpcChannel` | Async RPC with at-least-once delivery and retry |

### Protobuf Serialization

| Component | Purpose |
|-----------|---------|
| `common.proto` | Shared endpoint types (ActorEndpoint, ActorAddress, Ipv4Endpoint, Ipv6Endpoint) |
| `frame.proto` | WireFrame transport format |
| `messages.proto` | System message types (Down, Exit, Link, Unlink, SpawnRequest, SpawnResponse) |
| `DefaultSerializer` | Protobuf-based encode/decode for all message types |
| `registrar.proto` | Registrar protocol messages (Register, Accept, Join, Leave, Resolve) |
| `registrar_serialization.hpp` | to_proto/parse helpers for registrar protobuf types |

### Supervision

- `OneForOneSupervisor` — only the failed child restarts
- `AllForOneSupervisor` — all children restart when one fails
- `SupervisorActor` — supervises children via strategy pattern
- `SelfSupervisingActor` — manages own children with policy (max_restarts, restart_interval)

## Build

```bash
# Configure and build
cmake -S . -B build -GNinja
ninja -C build

# Run discovered GTest cases via CTest
ctest --output-on-failure --parallel 8

# Run a single GTest binary or case
./build/tests/unit/core/test_unit_core
./build/tests/unit/core/test_unit_core --gtest_filter="*ActorId*"
```

### Build Options

| Option | Description |
|--------|-------------|
| `-DENABLE_TSAN=ON` | Enable ThreadSanitizer |
| `-DENABLE_ASAN=ON` | Enable AddressSanitizer |
| `-DENABLE_EXAMPLES=OFF` | Disable examples (default ON) |
| `-DENABLE_PROACTOR=ON` | Enable proactor backend (OFF=macOS default, ON=Linux default) |
| `-DENABLE_MEMORY_TRACKING=OFF` | Disable per-actor memory tracking (default ON) |
| `-DENABLE_MEMORY_DEBUG=ON` | Enable memory poisoning + canary verification (default OFF) |
| `-DENABLE_ACTOR_METRICS=OFF` | Disable actor-level metrics subsystem (default ON) |
| `-DENABLE_ACTOR_LOGGING=OFF` | Disable structured actor logging subsystem (default ON) |
| `-DENABLE_ACTOR_TRACING=OFF` | Disable distributed tracing subsystem (default ON) |
| `-DENABLE_CLI=OFF` | Disable interactive CLI subsystem (default ON) |
| `-DENABLE_COVERAGE=ON` | Enable gcov/llvm-cov style coverage instrumentation |

## Design Constraints

These constraints are not arbitrary — each enables a specific architectural property:

### No exceptions in hot path
Actor message handling is on the critical path. Throwing exceptions would impose try/catch overhead on every message dispatch. Instead, errors use `error` codes (returned via `result<T>`) and are handled through the supervision hierarchy. This keeps message dispatch predictable and allocation-free.

### No RTTI — TypeTag replaces it
Distributed actors cannot rely on C++ RTTI since actor instances cross process boundaries. A `TypeTag` enum (0-99 for system messages, 100+ for user types) identifies message types for serialization dispatch. This is also faster than `dynamic_cast` and works across the network.

### C++20 coroutines for actor suspend/resume
Actors spend most of their time waiting for messages or I/O. C++20 stackless coroutines allow actors to suspend without a full thread stack — thousands of actors can be multiplexed onto a small thread pool. This is the foundation of million-level concurrency.

### Header-only types, linked runtime
Actor types, behaviors, and message definitions are header-only templates — zero overhead, inlined by the compiler. The actor runtime (scheduler, event loop, connection pool) is compiled into a shared library. This separation means actors pay no abstraction cost while the runtime can evolve independently.

### constexpr opaque ID initialization
`ActorId`, `MessageId`, `AlarmHandle`, and timer IDs are aliases over the shared `Id<Tag, T>` template. They keep well-known IDs constant-initializable (e.g., `SpawnReceiverId`) while preventing accidental comparisons between unrelated identifier domains.

### Lock-free mailbox earned through testing
The mailbox uses a Vyukov MPSC queue with an edge-trigger `CAS` wakeup mechanism. This was designed through iterative testing rather than upfront theory — the "swap-in mailbox interface" means the implementation can be replaced if the lock-free approach proves problematic on new hardware.

### Minimal dependencies

System packages: **OpenSSL** (TLS), **Protobuf** (serialization). Vendored in `third_party/`: **llhttp** (HTTP parsing), **toml++ v3.4.0** (TOML config parsing). On Linux, **liburing** is optional for the proactor backend.

### LLVM coding standards
The codebase uses LLVM style (`clang-format`) with strict warnings (`-Wall -Wextra -Wpedantic`). This ensures the code is clean, portable, and compatible with the clang toolchain used for development.

## Project Structure

```
include/hpactor/
├── actor/          — Actor base classes, behaviors, typed actors, spawn, lifecycle, durable state
│   └── lifecycle/  — LifecycleActor, circuit_breaker, drain_policy, passivation, quarantine, shutdown
│   └── durable/    — DurableStateStore, InMemoryStateStore, FileStateStore
├── adt/            — Shared data structures (Id, NodeIdentity, MpscRingBuffer, StreamBuffer, DedupCache)
├── ai/             — AI accelerator subsystem (AcceleratorConfig, AcceleratorType, NodeResourceSummary)
├── cli/            — CLI subsystem (CliActor, CliSession, CliServerActor, CommandNode, Lexer, OutputFormatter, Pager, commands)
├── config/         — TOML config topology parser, binary format, actor factory registry
├── core/           — ActorSystem, registry
├── coroutine/      — CoroutineTask, CoroutineFramePool, MailboxAwaiter, TimerAwaiter
├── log/            — Structured logging (LogManager, LogRingBuffer, LogDrain, sinks, formatters)
├── mailbox/        — MPSCActorMailbox, MultiLaneQueue, DeliveryMode, DeadLetterQueue, overflow handlers, admission
├── metrics/        — MpscRingBuffer, MetricRegistry, Aggregator, OpenMetricsFormatter, MetricsActor
├── msg/            — Messaging primitives (TypeTag, MessageId, TypedMessage, FailureReason, DeliveryResult, Frame, fwd.hpp)
├── net/            — EventLoop, TLS, connection pool, registrar, UDP, gossip, reactor/proactor
├── process/        — Process daemonization (ProcessManager, WatchdogActor, HealthHttpServer, SyslogSink)
├── ref/            — Actor references (address, ref, proxy, cache)
├── rpc/            — Async RPC channel with retry, deadline, and timeout
├── sched/          — HybridScheduler, work queues, worker threads, work placement
├── spawn/          — AsyncActor for non-blocking remote spawn
├── supervision/    — OneForOne, AllForOne supervisors
├── timer/          — TimingWheel, CalendarQueue
├── tracing/        — Distributed tracing (TraceManager, TraceContext, exporters, sampler)
├── mem/            — Two-tier slab allocator, hibernation, compaction, observability
└── types/          — Fundamental types and endpoint definitions

src/
├── actor/          — ActorSystem, ActorDirectory, EventBasedActor, SpawnReceiver, ActorContext, AskManager
│   ├── lifecycle/  — LifecycleActor, PassivationManager, ShutdownCoordinator, quarantine
│   └── durable/    — FileStateStore, InMemoryStateStore implementations
├── adt/            — Shared runtime data-structure implementations (including DedupCache)
├── ai/             — Accelerator type registration, AI message registry
├── cli/            — CliActor, CliSession, CliServerActor, lexer, command_node, formatters (pretty/json/tabular), pager, commands
│   └── commands/   — actor_commands, ask_commands, dlq_commands, failure_commands, reliable_commands, scheduler_commands, and more
├── config/         — TOML parser, binary serializer/loader, factory registry, subsystem parsers
│   └── parsers/    — ai_accelerator, ask, dead_letters, delivery, mailbox, passivation, process, rate_limiting, topology
├── log/            — LogManager, LogDrain, sinks (stderr, file, rotating, syslog), formatters
├── metrics/        — MetricRegistry, Aggregator, OpenMetricsFormatter, MetricsActor
├── tracing/        — TraceManager, exporters (memory, JSON, OTLP), sampler, context parser
├── mailbox/        — LocalDeliveryEngine, BackpressureCoordinator, DeliveryPipeline, MemoryPressureMonitor, DeadLetterQueue
├── msg/            — Frame protobuf, Frame implementation, DeliveryReceipt, OutboundDeliveryTracker
├── net/            — EventLoop, TcpTransport, TLS, connection pool, UDP transport
├── process/        — ProcessManager, HealthHttpServer, WatchdogActor
├── ref/            — ActorRef, ActorProxy implementations
├── rpc/            — RpcChannel implementation
├── sched/          — HybridScheduler, work placement scheduler, actor execution engine
├── coroutine/      — CoroutineFramePool implementation
├── timer/          — TimingWheel, CalendarQueue implementations
├── mem/            — SegmentProvider, SlabCache, memory tracker, hibernation, guard pages
├── spawn.cpp       — AsyncActor implementation
└── actor_type_registry.cpp — ActorTypeRegistry implementation

protos/hpactor/
├── ai_resource.proto — AI accelerator resource descriptors
├── cli_messages.proto — CLI inspect/kill/list/stats/memory messages
├── common.proto    — Shared endpoint types
├── frame.proto     — WireFrame transport format (includes ACK/NACK frame types)
├── gossip.proto    — GossipMembership protocol (SWIM messages, piggyback, SyncRsp)
├── messages.proto  — System message types
└── registrar.proto — Registrar protocol messages

tools/toml-compiler/ — AOT compiler: TOML topology → binary format
tools/hpactor-cli/  — Standalone CLI client binary (UDS/TCP attach to daemon)
docs/architecture/production/ — Production reliability roadmap, missing design docs, and refined requirement backlog
tests/              — 39 GTest binaries across unit, integration, and system tiers; 271 test source files and 1,924 source-level GTest cases
examples/           — 12 API usage examples, including the full order platform scenario
apps/               — Complex demo apps: order_platform, edgeops_telemetry, cli_demo, bench_perf
third_party/        — Vendored dependencies (llhttp, toml++)
cmake/              — CMake modules (protobuf codegen, toml++ interface target)
deploy/systemd/     — systemd unit file + production config.toml for daemon mode
```

## Status

### Complete (39 GTest binaries, 1,924 test cases, 271 test source files)

- **Actor Core**: spawn, send, reply, behaviors, typed actors, proto actors, stateful actors
- **Unified Message Passing**: TypedMessage with sender address, reply routing, error replies
- **Actor References**: ActorRef (local/proxy variant), ActorRefCache (LRU resolution cache)
- **Shared ADTs**: Opaque typed identifiers, shared MPSC ring buffer, stream buffer, and node identity primitives
- **Scheduled Messages**: `context()->schedule(delay, msg)` for timer-based self-delivery with `AlarmHandle` cancellation
- **Supervision**: OneForOne, AllForOne, SupervisorActor, SelfSupervisingActor
- **Scheduling**: HybridScheduler with work-stealing + EDF + timing wheel + coroutine frame pool
- **Coroutines**: CoroutineTask, MailboxAwaiter, TimerAwaiter, YieldAwaiter
- **Delivery Semantics**: delivery modes, delivery deadlines with TTL enforcement, receiver deduplication cache, unified DeliveryResult outcome type, and canonical failure mapping
- **Mailbox**: MultiLaneQueue lock-free multi-lane queue, MPSCActorMailbox (edge-triggered CAS), bounded admission, overflow handlers, rate limiter + admission policies, backpressure signals
- **Dead-Letter Queue**: Bounded record capture with reason/source tracking, payload sampling, snapshot and pop APIs
- **Memory Management**: Two-tier slab allocator (mmap → thread-local slabs), typed regions with pressure admission, hibernation with ZRAM hints, compaction with fragmentation budget, per-actor observability, memory poisoning + canaries + guard pages
- **Network**: TLS 1.3, connection pooling, UDS support, reactor/proactor backends
- **HTTP Gateway**: HTTPGatewayActor with route registration, HttpClient for outbound requests, request/reply correlation
- **Service Discovery**: Pluggable IServiceDiscovery with 4 backends (Gossip SWIM, UdpRegistrar, Hybrid, Static) + ActorLocationCache
- **Remote Spawn**: AsyncActor with spawn_remote()/spawn_remote_async(), ActorTypeRegistry, cross-process message routing
- **RPC**: Async RPC channel with at-least-once delivery, retry, and timeout
- **Serialization**: Protobuf-based for all system messages (WireFrame, Down, Exit, Link, Unlink, Spawn)
- **TOML Config Topology**: Declarative topology bootstrapping with templates, imports, AOT binary compilation
- **Actor Metrics**: Out-of-band ring buffer instrumentation, OpenMetrics `/metrics` endpoint for Prometheus/Grafana
- **CLI Interactive**: Trie-based command tree with InspectState introspection, paged output, pluggable formatters (Pretty/JSON/Tabular)
- **Structured Logging**: MPSC ring buffer, batched drain, pluggable sinks (stderr, file, rotating), text/JSON formatters
- **Distributed Tracing**: W3C TraceContext propagation, actor receive spans, memory/JSON/OTLP exporters, parent-based sampling
- **Actor Lifecycle**: LifecycleActor mixin with constexpr state machine, message gate, supervision integration
- **Failure Semantics**: FailureReason, FailureSource, FailureEnvelope, mappings from mailbox/errors/spawn/DLQ, and CLI failure commands
- **Actor Quarantine**: Opt-in quarantine policies, per-actor quarantine escalation, and quarantine metrics
- **Circuit Breaker (ACT-005)**: Per-actor CircuitBreakerTracker with Closed→Open→HalfOpen state machine, failure-rate EMA, cooldown/probe admission, quarantine escalation
- **Actor Rate Limiting & Admission (ACT-006)**: ActorRateLimiter + 5 IAdmissionPolicy implementations (size limit, type filter, sender filter, priority threshold, per-sender rate), wired into mailbox admission gate
- **Ask/Timeout Policy (ACT-007)**: RequestHandle<T> move-only future, RequestTimeout specification, AskManager subsystem, ActorContext::ask() API, RPC deadline enforcement across retries
- **Actor Passivation (ACT-008)**: PassivationManager with IActorRoute abstraction, InMemoryStateStore/FileStateStore, MemoryPressureMonitor, new kPassivating/kPassivated lifecycle states, TOML config
- **Delivery Result API (MSG-002)**: DeliveryResult unified outcome type for try_send, delivery metrics aggregation, and remote delivery result tests
- **Deadline/TTL Enforcement (MSG-003)**: Per-message TTL enforced at enqueue, handler receive, and coroutine awaiter; expired messages recorded to DLQ; configurable default_message_ttl_ms
- **Graceful Shutdown**: DrainPolicy, phase-machine coordinator, CLI drain/stop commands, TOML config
- **Deterministic Fault Injection**: 80 fault injection sites across 14 domains, seed-replayable schedules, FAULT_INJECT macro, CLI /fault commands
- **Actor System Decomposition**: ActorDirectory, LocalDeliveryEngine, BackpressureCoordinator, ShutdownCoordinator extracted from monolithic ActorSystem
- **Msg Subsystem**: 14 headers in `msg/` with fwd.hpp forward-declarations hub — TypeTag, MessageId, TypedMessage, FailureReason, DeliveryResult, Frame, and more
- **Deterministic Test Support**: Scheduler worker pause/resume/step driver plus CI-oriented test design constraints for race-prone subsystems
- **Google Test Harness**: Vendored Google Test, tiered test binaries (39 GTest binaries, 1,924 test cases), discovered GTest cases, and system-level network/registrar coverage
- **CI and Coverage**: GitHub Actions CI, coverage workflow, README badges, and stabilized gcc-debug / clang-release test paths
- **Complex Demo Apps**: Order Platform (multi-actor order pipeline with DLQ, tracing, HTTP gateway, remote spawn), EdgeOps Telemetry (IoT edge telemetry with alerts, backpressure, DLQ evidence), CLI Interactive Demo (7 actor types exercising full CLI command surface), Bench Perf (coordinator/worker/collector/hot actor throughput and latency benchmarks)
- **Daemon Service (sys-284)**: ProcessManager (double-fork + systemd notify), CliServerActor (UDS/TCP socket server), CliSession (transport-agnostic command processor), hpactor-cli standalone binary, HealthHttpServer, WatchdogActor, SyslogSink, systemd unit with security hardening, self-registering TOML `[system.process]` parser
- **AI Accelerator Subsystem**: AcceleratorConfig with 7 device types (CPU/GPU/NPU/FPGA/DSP/Custom), NodeResourceSummary, ai_resource.proto wire format, self-registering TOML `[ai.accelerators]` parser, `ENABLE_AI_ACCELERATORS` build gate
- **Scheduler Hardening**: Lost-wakeup window closed on x86_64 (futex-based CV wait), rate-limiter spin loop capping, adaptive worker idle model (polling vs CV), real kernel TID display, TimingWheel cascading edge case fixes
- **Mailbox Thread Safety**: Prearm race detection and fix, formal validation test suite for MPSC correctness properties, ARM64 concurrency documentation
- **Reliable Messaging Primitives (MSG-004)**: ACK/NACK frame types, RetryPolicy with exponential backoff + jitter, OutboundDeliveryTracker, DeliveryReceipt, DeliveryPipeline, IDurableDeliveryStore interface, `/reliable` CLI commands

### Designed / Backlogged

- **Production Reliability Plane**: Data/control/operations plane roadmap for 24x7 operation
- **Reliable Messaging Completion**: Durable outbox/inbox recovery, retry exhaustion policy, persistent delivery tracker (ACK/NACK frame types, RetryPolicy, OutboundDeliveryTracker, DeliveryReceipt, and DeliveryPipeline implemented)
- **Durable Actor State**: Snapshot, event sourcing, recovery (InMemoryStateStore and FileStateStore implemented for passivation; general-purpose durable actor state remains backlog)
- **Cluster Control**: Node failure model, node quarantine/fencing, sharding, placement, handoff, and rolling upgrade design
- **Production Operations**: Authenticated admin API, security/audit, config reload, chaos/soak/fuzz testing (health endpoint partially implemented via HealthHttpServer)

### Next Steps

- Production reliability plane: reliable messaging completion (durable outbox/inbox), security (mTLS, auth, audit), operations-plane admin API
- AI accelerator subsystem: compute offload dispatch, accelerator-aware scheduler placement, resource-aware load balancing
- Cluster control plane: node failure model (node quarantine/fencing), protocol/feature negotiation for rolling upgrades, sharding/placement/rebalance protocol
- Production operations: authenticated admin API, dynamic config validation/diff/reload, chaos/soak/fuzz test lanes
- Typed RPC API (`call<Request, Response>` with serialization)
- Argument deserialization for passing constructor args through remote spawn
- Proactor backend production hardening (IoUringBackend, GcdBackend)
- Tiny-block optimization for 32B size class in slab allocator
- Runtime configuration via environment variables for memory limits
