# HPActor Project Memory

This project has a persistent memory system in `.claude/projects/-Users-skg7on-Workspace-Projects-HPActor/memory/`.

## Quick Reference

| Topic | File | Description |
|-------|------|-------------|
| Architectural Decisions | `architectural_decisions.md` | Actor model, type system, supervision strategy |
| Implemented Features | `implemented_features.md` | Complete implementation - what's built |
| Project Status | `project_status.md` | Current phase, next steps, build commands |
| Production Reliability Architecture | `docs/architecture/production/production-reliability-plane.md` | 24x7 reliability roadmap and production architecture backlog |

## Current State

**Production Reliability Architecture:** ✅ Design Docs Complete (2026-05-10)
- New production architecture area: `docs/architecture/production/`
- Top-level roadmap: `production-reliability-plane.md`, organizing the next evolution into data plane, control plane, and operations plane.
- Summary backlog: `architecture-requirement-backlog.md`.
- Refined feature-gap backlog: `feature-gap-refined-requirement-backlog.md`, with requirement cards by subsystem covering gap, architecture requirement, runtime contract, dependencies, acceptance evidence, observability, and tests.
- Missing design docs added: actor delivery semantics, cluster failure model, dead-letter queue, cluster sharding/placement, reliable messaging, durable actor state, graceful shutdown/rolling upgrade, security, operations/SRE, dynamic config/parser IoC, and chaos/reliability testing.
- Recommended production milestone: Production Reliability Plane foundation, starting with delivery semantics, bounded mailboxes/backpressure, DLQ, tracing, health, and graceful shutdown.
- Status: architecture and backlog only; runtime implementation is still pending.

**Failure Envelope Phase 1:** ✅ Complete (2026-05-20, 9 commits)
- `FailureReason` enum (23 values in 10 semantic ranges) + `FailureSource` enum (12 subsystem origins) in `include/hpactor/types/failure_reason.hpp`.
- `FailureEnvelope` struct with full correlation metadata (actor_id, sender, receiver, message_id, trace, retryable, timestamp, source, detail) in `include/hpactor/types/failure_envelope.hpp`.
- `make_failure_envelope()` factory function with monotonic clock timestamp capture.
- `EnqueueResult::failure_reason()` — maps `EnqueueResultCode` → `FailureReason`.
- `error::failure_reason()` — maps `errors::` codes → `FailureReason`.
- `try_deliver_local()` builds `FailureEnvelope` on both failure paths (ActorNotFound + mailbox rejection), emits `kDeliveryFailure` metric event and structured log warning.
- `kDeliveryFailure = 20` added to `MetricEventType`; aggregator stub added.
- 2 new test suites: `test_failure_reason` (retryable, to_string, enum mapping) and `test_failure_envelope` (construction, factory, truncation, null termination).
- 153 tests pass (150 existing + 2 new: test_failure_reason, test_failure_envelope).
- Design spec: `docs/architecture/production/structured-failure-envelope-design.md`. Implementation plan: `docs/superpowers/plans/2026-05-20-failure-envelope-phase1.md`.
- Phase 2 (DLQ integration), Phase 3 (RPC + spawn), Phase 4 (CLI) remain.
- Branch: `task/failure-envelope-spec`.

**Service Discovery:** ✅ Complete (2026-05-08, 15 commits, ~2000 lines)
- Pluggable `IServiceDiscovery` interface — 4 backends: `UdpRegistrar` (same-host, refactored), `GossipMembership` (cross-server SWIM protocol), `HybridDiscovery` (composes both), `StaticDiscovery` (fixed topology)
- `ActorLocationCache` — TTL cache for ActorId → EndPoint resolution, integrated into `ActorProxy::send()`
- `GossipMembership` — SWIM protocol (ping/ack/PingReq/indirect probes), suspicion/death state machine, incarnation-based conflict resolution, protobuf wire format (`gossip.proto`), async UDP via EventLoop
- `GossipConfig` — gossip_port, protocol_period (1s), ping_timeout (200ms), suspicion_timeout (3s), dead_timeout (30s), fanout=3, indirect_probes=3, seeds
- ActorSystem integration — backend selection in constructor, `on_node_dead()` death propagation, cache purge timer
- TOML `[system.discovery]` parsing — backend selection + gossip config
- 2 new test suites: test_gossip_membership (15 tests), test_service_discovery (13 tests)
- Design docs: core concept, architecture, deployment scenarios, detailed spec, implementation plan

**TOML Config Topology:** ✅ Complete (2026-05-04, 14 commits, ~2700 lines)
- Declarative actor topology bootstrapping — describe the full actor tree in TOML
- `TopologyModel` — ActorDef, DispatcherDef, SystemDef, ResourceSpec data structures
- `ActorFactoryRegistry` — singleton mapping behavior name strings to factory functions
- `HPACTOR_REGISTER_ACTOR` macro — static registration before main()
- `TomlParser::parse()` — import resolution (glob), template inheritance (deep merge), validation, topological sort (Kahn's algorithm)
- `BootstrapEngine` — `spawn_configured()`, dispatcher creation, behavior validation, batch spawn in DAG order
- `SystemInitTag` (TypeTag 12) — broadcast after full topology spawn to gate external traffic
- `ConfigurableActor` concept — per-actor `configure_from_args()` interface
- AOT compiler — C++ executable (`hpactor_toml_compiler`) linking hpactor_lib, shares parsing logic
- Binary format — custom mmap-friendly format with string table for zero-copy loading
- `ActorSystem::load_topology()` — end-to-end convenience: parse TOML → spawn → SystemInit
- 4 new test suites: factory registry (6), parser (13), bootstrap engine (7), binary roundtrip (3)
- `toml++` v3.4.0 as header-only FetchContent dependency

**Actor Core Framework:** ✅ Complete (Phase A-G, 65 tests passing)

**Link/Monitor:** ✅ Complete (2026-04-29)
- `link_to()`/`unlink_from()` — bidirectional death sharing via LinkMsg/UnlinkMsg
- `monitor()`/`demonitor()` — one-way death watching
- Death propagation via `on_exit()` with `DownMsg` to linked + monitored actors
- System message dispatch in `EventBasedActor::receive()` intercepts LinkMsg/UnlinkMsg/DownMsg

**Memory Management:** ✅ Complete (2026-05-03, 18 commits, 83 tests passing)
- Two-tier slab allocator — Tier 0: mmap-backed SegmentProvider (2MB segments), Tier 1: per-thread SlabCache with bump+freelist (32B–4KB size classes)
- AllocHeader (32B) + CanaryFooter (8B) on every block — owner ActorId, incarnation counter, magic canary, generation
- Lock-free CAS freelist for block recycling, CAS-based MPSC TelemetryRingBuffer for allocation events
- MemoryTracker — per-actor shadow counters (64B-aligned, lock-free), 1M actor capacity
- Typed memory regions: kActor, kMessage, kCoroutine, kNetwork, kInternal, kHibernate
- ThreadLocalAllocator per WorkerThread, global mem::allocate()/mem::deallocate() API
- Memory poisoning (0xAA) + canary verification (debug mode), guard pages with SIGSEGV/SIGBUS handler
- Hibernatable interface, HibernationRegistry (ActorId → serialized buffer), ActorState::kHibernating
- CompactionManager with generation tracking and 5% fragmentation budget
- ZramManager — MADV_PAGEOUT/COLD/WILLNEED hints for ZRAM integration
- MPSCActorMailbox refactored to use custom allocator (placement new + mem::deallocate)
- Build flags: ENABLE_MEMORY_TRACKING (ON by default), ENABLE_MEMORY_DEBUG (OFF)

**HTTP Protocol:** ✅ Complete (2026-04-30)
- HTTP parser, serializer, server (test_http_parser, test_http_serializer, test_http_server)
- Fundamental types: ActorId, error, Clock, AlarmHandle, TraceContext, MessageId, result<T>
- Actor base classes: abstract_actor, local_actor, event_based_actor
- ActorContext, ActorSystem, actor_registry
- Blocking actors: blocking_actor, scoped_actor
- Stateful actor: stateful_actor<T>
- Typed actors: typed_event_based_actor, typed_behavior
- ActorMailbox integration
- Supervision: OneForOne, AllForOne, supervisor_actor, self_supervising_actor

**Network Layer:** ✅ Complete (Phase 4-5, optional TLS 2026-04-22, comm-endpoint refactor 2026-04-23, registrar protobuf 2026-04-25)
- TlsContext — certificate loading, RSA signing, pre-master secret decryption
- Connection — abstract base, owns fd, local_endpoint, remote_endpoint, EventLoop*; handle_read() pure virtual (no args), no framing assumptions
- TlsConnection — TLS state machine, AES-256-CBC encryption, inherits from Connection
- PlainConnection — raw TCP socket, 4-byte length-prefixed framing, inherits from Connection
- ConnectionPool — standalone class (not a Connection), dynamic pooling per node, round-robin, exponential backoff
- TcpTransport — uses ConnectionPtr, creates PlainConnection or TlsConnection based on pool_config_.use_tls; connect() returns individual Connection (not pool)
- PoolConfig::use_tls defaults to false (plain text is default)
- EventLoop timer support — EVFILT_TIMER/timerfd for reconnect backoff
- EventLoop backend fallback — EpollBackend (Linux), KqueueBackend (macOS) with explicit run()/stop()
- UdpRegistrar — UDP-based node discovery with server/client dual mode
- HostResolver — DNS resolution with caching
- NodeRegistry — registry of known nodes with static routes
- RegistrarServer — TCP server for node registration, heartbeat, broadcasts
- RegistrarClient — TCP client with failover, local IP detection, AcceptorInfo
- **UNIX Domain Socket Support (2026-04-25)** — `listen_unix_domain()` in Acceptor, `connect_unix_domain()` in TcpTransport, registry-driven UDS path lookup with TCP fallback, UDS path derivation utility with `/tmp/hpactor/<node_id>.sock` convention
- **Registrar Protobuf Serialization (2026-04-25)** — `registrar.proto` with PbRegisterPayload, PbAcceptPayload, PbNodeJoinPayload, PbNodeLeavePayload, PbErrorPayload, PbResolveQueryPayload, PbResolveResponsePayload; `registrar_serialization.hpp` with to_proto/parse helpers; RegistrarServer/RegistrarClient updated to use protobuf instead of manual byte serialization
- **Async UDP (2026-04-25)** — OpCompletion extended with src_addr/src_addr_len for UDP recvfrom; UdpRegistrar async UDP via EventLoop edge-triggered polling and async_sendto
- **RegistrarServer refactor (2026-04-26)** — removed background polling thread; RegistrarServer now uses EventLoop's completion callback for send routing; added SO_REUSEADDR and error handling for UDP bind
- **ActorRef remote send (2026-04-26)** — ActorRef::send() now calls ActorProxy::send() for remote actors instead of placeholder comment
- **liburing optional (2026-04-26)** — on Linux, liburing is now optional; if not found, build uses epoll backend only without external dependencies

**CommunicationEndpoint Refactor** ✅ Complete (2026-04-23, 50 tests passing)
- NodeId (string "host:port") replaced with CommunicationEndpoint (std::variant<Ipv4Endpoint, Ipv6Endpoint>)
- Ipv4Endpoint stores uint32_t addr, uint16_t port in **network byte order** for efficient socket operations
- Ipv6Endpoint stores std::array<uint8_t, 16> addr, uint16_t port in network byte order
- ActorAddress now holds CommunicationEndpoint directly (replaced NodeId node_id field)
- endpoint_ops::parse_endpoint(NodeId) converts string to endpoint, endpoint_ops::to_string() converts back
- Binary serialization: 0x04 prefix + 7 bytes for IPv4, 0x06 prefix + 19 bytes for IPv6
- is_local() uses loopback detection (127.0.0.1 in network byte order = 0x7F000001)
- Fixed ARM Mac bug: inet_pton returns host byte order, required htonl() conversion
- ActorAddress{} default initializes to loopback (127.0.0.1:0) to match parse_endpoint("")

**Phase 7: Async RPC Channel** ✅ Complete (48 tests)
- RpcChannel — async RPC with at-least-once delivery, retry on timeout
- RpcFuture<bytes> — future wrapper with timeout-enforced get()
- Frame flags: RpcRequest, RpcResponse, RpcIdempotent
- ConnectionPool::set_rpc_handler() for RPC response routing
- Transport::set_rpc_handler() interface propagated to TcpTransport
- ActorContext::rpc() and ActorSystem::rpc_channel() for non-actor thread access

**Phase 6: Remote Actor Spawn** ✅ Complete (34 tests passing)
- AsyncActor handle for non-blocking spawn with get(), ready(), cancel()
- SpawnRequest/SpawnResponse message types
- ActorTypeRegistry for registering spawnable actor types
- SpawnReceiver system actor for handling spawn requests
- ActorSystem::spawn_remote() and spawn_remote_async()
- Well-known system ActorIds (SpawnReceiverId, SystemActorType)

**Examples:** ✅ Complete
- `examples/01_echo_actor.cpp` — EventBasedActor, make_behavior(), become()
- `examples/02_counter_stateful.cpp` — StatefulActor<T>, state management
- `examples/03_typed_calculator.cpp` — typed_actor<>, TypedBehavior
- `examples/04_supervision_tree.cpp` — OneForOne/AllForOne, SupervisorActor
- `examples/05_ping_pong.cpp` — Actor communication, ScopedActor, linking
- Built via `ENABLE_EXAMPLES` CMake option (default ON)

**Scheduling Subsystem:** ✅ Complete (Phase 0-7, 2026-04-15)
- `ChaselevDeque<T>` — Lock-free work-stealing deque (LIFO owner pop, FIFO thief steal)
- `MultiPriorityWorkQueue` — Array of ChaseLev deques, one per priority level (0=highest)
- `EDFQueue` — Earliest Deadline First min-heap, O(log n) push/pop, FIFO tiebreaker
- `A2WS` — Adaptive Two-Level Work Stealing with pool-based locality
- `TimingWheel` — Hierarchical timer wheel (O(1) insert/cancel), 4 levels, cascading
- `CoroutineFramePool` — Lock-free stack pool for coroutine frames, O(1) acquire/release
- `HybridScheduler` — Work-stealing scheduler with IScheduler interface, wired to ActorSystem
- `WorkerThread` — Per-thread worker with local queue and frame pool integration
- `IScheduler` interface: `notify_ready()`, `notify_idle()`, `schedule_after()`, `schedule_every()`, `cancel_timer()`, `worker_count()`
- Timer advancement thread with proper cancellation for recurring timers
- `ActorState` — Atomic state machine (Idle/Ready/Running/IOWaiting/Terminated) with CAS transitions
- `CoroutineTask` / `CoroutinePromise` — C++20 coroutine handle wrapper for actor coroutines
- `MailboxAwaiter`, `TimerAwaiter`, `BlockingMailboxAwaiter` — awaiters for co_await patterns
- `MPSCMailbox<T>` — Vyukov lock-free MPSC queue (wait-free enqueue, lock-free dequeue), includes cyclic queue fix when returning last element
- `execute_actor()` dispatch layer for coroutine resumption with state transitions

**Actor Metrics:** ✅ Complete (2026-05-05, 8 commits, 90 tests passing)
- Out-of-band lock-free ring buffer instrumentation — CAS-based `MpscRingBuffer<T>` (extracted from `TelemetryRingBuffer`)
- 32-byte `MetricEvent` schema: 10 event types (mailbox enqueue/dequeue, processing latency, lifecycle, scheduler dispatch/steal, supervision restart, memory alloc/free)
- `MetricRegistry` — Counter, Gauge, Histogram with atomic updates and snapshot
- `Aggregator` — event-to-metric dispatch with actor_type label caching via `ActorSystem::get_actor()`
- `OpenMetricsFormatter` — `text/plain; version=1.0.0` with `# HELP`/`# TYPE`/`_bucket`/`_sum`/`_count`/`# EOF`
- `MetricsActor` — EventBasedActor with `on_request<MetricsRequest, MetricsResponse>`, drains ring buffer on each `/metrics` scrape
- Integration: MPSCActorMailbox (enqueue/dequeue events), EventBasedActor (processing latency + on_exit terminate), ActorSystem (spawn events, MetricsActor wire-up, ring buffer pass-through), HybridScheduler (dispatch + steal events), SupervisorActor (restart events)
- `virtual type_name()` on `AbstractActor` for metrics labeling, `virtual set_metrics_ring_buffer(void*)` for pointer pass-through without RTTI
- TOML `[system.metrics]` config: enabled, ring_buffer_capacity, metrics_path
- `ENABLE_ACTOR_METRICS` CMake option (default ON)
- `MetricsRequest`/`MetricsResponse` protobuf messages, `MetricsRequestTag=13`/`MetricsResponseTag=14` TypeTags
- 2 new test suites: `test_metrics_registry`, `test_metrics_integration`

**Actor CLI Interactive:** ✅ Complete (2026-05-06, 12 commits, ~1900 lines)
- Trie-based `CommandNode` registry — multi-level commands (`/actor <id> show`) with fuzzy suggestion (Levenshtein distance ≤2)
- `CliActor` — `DaemonActor` subclass with dedicated I/O thread, blocks on stdin for interactive input
- `Lexer` — whitespace tokenizer with `--flag`/`--flag value`/`"quoted strings"` support
- `OutputFormatter` — pluggable renderers: `PrettyFormatter` (ANSI box-drawing), `JsonFormatter` (machine-readable), `TabularFormatter` (grep-friendly)
- `Pager` — interactive paging state machine for `/actor list` with n/p/q/search/goto navigation
- `InspectStateRequest`/`InspectStateReply` — thread-safe actor introspection via message passing (CLI never reads actor memory directly)
- `to_metadata()` / `serialize_state()` / `mailbox_snapshot()` — virtual interface on `AbstractActor` for inspectable state
- System message dispatch in `EventBasedActor::receive()` for CLI TypeTags (0x50-0x5F)
- `InspectStateRequest`, `KillRequest`, `ListActorsRequest`, `SystemStatsRequest`, `MemoryStatsRequest` protobuf messages
- TOML `[system.cli]` config: enabled, listen_path, tcp_port, default_format, page_size
- `ENABLE_CLI` CMake option (default ON), `HPACTOR_ENABLE_CLI` compile-time guard
- 5 new test suites: test_lexer, test_command_node, test_formatters, test_pager, test_cli_integration
- CLI is opt-in at runtime (`CliConfig::enabled = false` by default) — explicit enable via TOML or Config

**Structured Logging:** ✅ Complete (2026-05-11, 20 commits, 7 test files)
- `LogRingBuffer` — MPSC lock-free bounded ring buffer for log entries
- `Logger` — timestamp capture, structured log entries with categories and levels
- `LogDrain` — consumer thread with batching and sink writes
- `ILogSink` — pluggable sinks: `StderrSink`, `FileSink`, `RotatingFileSink` (size-based rotation), `MemorySink` (for testing)
- `TextLogFormatter`, `JsonLogFormatter` — human-readable and machine-parseable output
- `LogManager` — owns config, ring buffer, drain, and sinks; wired into ActorSystem lifecycle
- TOML `[system.log]` config: level, sinks, file paths, rotation settings
- Header-only `include/hpactor/log/`, compiled `src/log/`
- 7 test files: test_log_ring_buffer, test_log_level, test_log_category, test_log_formatter, test_log_config, test_log_sinks, test_log_integration

**Distributed Tracing:** ✅ Complete (2026-05-12, ~1800 lines, 12 test files)
- W3C-compatible `TraceContext` — 16-byte TraceId, 8-byte SpanId, TraceFlags (sampled flag)
- Actor receive spans (consumer kind) with RAII `SpanGuard` — automatic start/end around message handlers
- Local send/reply propagation via `ActorContext` current trace scope
- Remote frame propagation — `PbTraceContext` in `frame.proto`, encoded in Frame header
- `TraceManager` — parent-based sampling decision, bounded MPSC ring buffer, drain thread, memory exporter
- `TraceExporter` interface — `MemoryExporter` for testing/inspection
- `Span` — name, kind (producer/consumer/client/server/internal), attributes, status, events, links
- TOML `[system.tracing]` config: enabled, sampling_rate, exporter, ring_buffer_capacity
- `ENABLE_ACTOR_TRACING` CMake option (default ON)
- 12 test files: test_w3c_trace_context, test_trace_context, test_trace_actor_context, test_trace_actor_system, test_trace_manager, test_trace_exporters, test_trace_message_propagation, test_trace_wire_propagation, test_trace_typed_message, test_trace_rpc, test_sampler, test_trace_config_smoke

**Actor Lifecycle:** ✅ Complete (2026-05-13, PR #94)
- `LifecycleActor` mixin — opt-in lifecycle state machine
- `LifecycleState` enum: Created → Starting → Running → Stopping → Stopped; plus Failed, Terminated
- Constexpr `StateDef` transition table — compile-time state transition definitions
- Message gate — rejects messages during non-Active states (Created, Starting, Stopping, Stopped, Failed)
- Supervision integration — drives FAILED→STARTING transition on restart
- Metrics events for lifecycle transitions
- 2 test files: test_lifecycle_state (12 state machine tests), test_lifecycle_actor

**Actor State Transfer:** ✅ Complete (2026-05-13, PR #95)
- State transfer for non-coroutine actors — serialized state handoff during migration
- Integration with hibernation and lifecycle subsystems

**Bounded Mailboxes & Dead-Letter Queue:** ✅ Complete (2026-05-14, PR #96 prerequisites)
- `BoundedMailbox` — bounded capacity with overflow policies: Block, DropHead, DropTail, DeadLetterQueue
- `DeadLetterQueue` actor — retention policy (max records, max age), per-actor observability, replay capability
- Mailbox backpressure signals — push-back from full mailbox to producer via overflow policy
- `MailboxPolicy` config — TOML-driven per-actor mailbox sizing and overflow policy
- 5 new test files: test_bounded_mailbox, test_dead_letter_queue, test_mailbox_overflow_policies, test_mailbox_backpressure_stress, test_mailbox_policy

**Graceful Actor Stop & System Shutdown:** ✅ Complete (2026-05-15, PR #96)
- `DrainPolicy` — Complete (drain all in-flight), Drop (discard), Timeout (deadline-based)
- `DrainConfig` — per-actor drain configuration with policy and timeout
- Drain-aware message processing — stops accepting new messages during drain; in-flight messages complete normally
- Drain timeout via `TimingWheel` — configurable deadline for in-flight message completion
- `ActorSystem::shutdown()` — phase-machine coordinator: user actors drain first, system actors drain last
- TOML `[system.shutdown]` config: drain_timeout, stop_timeout, phase ordering
- CLI `/system drain` and `/system stop` commands
- DLQ `DrainTimeout` and `DrainPolicyDrop` dead-letter reasons
- `on_drain_timeout()` hook for custom drain timeout handling
- 4 test files: test_drain_policy, test_drain_timeout, test_drain_integration, test_shutdown_coordinator

**Deterministic Scheduler Worker-Control API:** ✅ Complete (2026-05-16, PR #104)
- `scheduler_test_driver.hpp` support header (61 lines) — pause/resume/step workers for deterministic tests
- `test_scheduler_control.cpp` (169 lines) — validates worker-control API
- Refactored worker tests for deterministic scheduling

**Full-Featured Example:** ✅ Complete (2026-05-16, PR #102)
- Order platform — coordinator, inventory, payment, fulfillment actors
- Happy path scenario — order flows through all actors successfully
- Failure scenarios — overload (bounded mailbox), missing-route, worker-crash
- CLI integration — inspect order state via CLI commands
- `test_order_platform_messages.cpp` — validates message flow and failure handling
- Follow-up issues #97-#100 implemented (PR #111 design specs + #47cb85e implementation)

**Low-Coverage Test Additions:** ✅ Complete (2026-05-17, PR #110)
- Extended tests for: ConnectionPool, HybridDiscovery, CliActor, LineEditor, Supervision, GuardPage, ActorProxy, SpawnReceiver, ScopedActor, LocalActor, MetricsAggregator, LogSink, WorkerThread, CoroutineFramePool
- 13 test files extended with construction, edge-case, and utility tests

**Coverage Badge:** ✅ Complete (2026-05-17, PRs #106-108)
- Automated coverage reporting with badge in README

**Tests:** ✅ 140 tests passing (152 test source files)
- 16 test subdirectories: actor (29), cli (6), config (7), core (2), examples (1), log (7), mailbox (11), mem (14), metrics (2), net (19), ref (3), rpc (1), sched (18), spawn (5), supervision (5), tracing (12), +1 top-level

**Documentation:** ✅ Complete
- Architecture: `docs/architecture/production/production-reliability-plane.md` (24x7 production reliability roadmap)
- Architecture: `docs/architecture/production/architecture-requirement-backlog.md` (summary production requirement backlog)
- Architecture: `docs/architecture/production/feature-gap-refined-requirement-backlog.md` (detailed feature-gap requirement backlog)
- Architecture: `docs/architecture/production/actor-delivery-semantics-design.md` (delivery result, TTL, retry, duplicate semantics)
- Architecture: `docs/architecture/production/dead-letter-queue-design.md` (DLQ retention, replay, observability)
- Architecture: `docs/architecture/production/cluster-failure-model-design.md` (node state, partitions, quarantine, fencing)
- Architecture: `docs/architecture/production/cluster-sharding-placement-design.md` (shards, placement, handoff)
- Architecture: `docs/architecture/production/reliable-messaging-design.md` (ACK/NACK, retry, dedup, durable outbox/inbox)
- Architecture: `docs/architecture/production/durable-actor-state-design.md` (snapshot, event sourcing, recovery)
- Architecture: `docs/architecture/production/graceful-shutdown-rolling-upgrade-design.md` (drain, leave, compatibility)
- Architecture: `docs/architecture/production/security-architecture-design.md` (mTLS, authorization, audit)
- Architecture: `docs/architecture/production/operations-sre-design.md` (health, admin API, incident timeline)
- Architecture: `docs/architecture/production/dynamic-config-parser-ioc-design.md` (subsystem-owned TOML parsing and reload classes)
- Architecture: `docs/architecture/production/chaos-reliability-testing-design.md` (fault injection, chaos, soak, fuzz, compatibility)
- Tutorial: `docs/superpowers/tutorials/actor-framework-tutorial.md`
- Spec: `docs/superpowers/specs/2026-04-11-actor-design.md`
- Plan: `docs/superpowers/plans/2026-04-11-actor-core-impl.md`
- Spec: `docs/superpowers/specs/2026-04-14-registrar-refactor-design.md` (registrar bug fixes)
- Plan: `docs/superpowers/plans/2026-04-14-registrar-refactor-impl.md`
- Spec: `docs/superpowers/specs/2026-04-14-event-loop-backend-fallback-design.md`
- Plan: `docs/superpowers/plans/2026-04-14-event-loop-backend-fallback-impl.md`
- Spec: `docs/superpowers/specs/2026-04-15-coroutine-scheduling-design.md`
- Plan: `docs/superpowers/plans/2026-04-15-coroutine-scheduling-impl.md`
- Spec: `docs/superpowers/specs/2026-04-20-rpc-channel-design.md` (async RPC channel)
- Plan: `docs/superpowers/plans/2026-04-20-rpc-channel-impl.md`
- Spec: `docs/superpowers/specs/2026-04-22-optional-tls-plaintext-design.md` (optional TLS, plain text default)
- Plan: `docs/superpowers/plans/2026-04-22-optional-tls-plaintext-connection.md`
- Spec: `docs/superpowers/specs/2026-04-25-unix-domain-socket-support-design.md`
- Plan: `docs/superpowers/plans/2026-04-25-unix-domain-socket-support-impl.md`
- Spec: `docs/superpowers/specs/2026-04-25-registrar-protobuf-async-udp-design.md` (registrar protobuf + async UDP)
- Plan: `docs/superpowers/plans/2026-04-25-registrar-protobuf-async-udp-plan.md`
- Spec: `docs/architecture/memory/memory-management-architecture-design.md` (memory management)
- Spec: `docs/architecture/core/actor-toml-config-core-concept.md` (TOML config philosophy)
- Spec: `docs/architecture/core/actor-toml-config-architecture.md` (TOML config detailed spec)
- Plan: `docs/superpowers/plans/2026-05-03-toml-config-topology-impl.md` (TOML config implementation)
- Plan: `docs/superpowers/plans/2026-05-03-memory-management-impl.md`
- Architecture: `docs/architecture/actor/actor-metrics-design.md` (metrics core concept)
- Architecture: `docs/architecture/actor/cli-interactive-design.md` (CLI interactive core concept)
- Spec: `docs/superpowers/specs/2026-05-04-actor-metrics-design.md` (metrics detailed spec)
- Spec: `docs/superpowers/specs/2026-05-05-actor-cli-interactive-design.md` (CLI interactive detailed spec)
- Plan: `docs/superpowers/plans/2026-05-04-actor-metrics-impl.md` (metrics implementation plan)
- Plan: `docs/superpowers/plans/2026-05-05-actor-cli-interactive-impl.md` (CLI interactive implementation plan)

## Key Decisions

- Event-based actors (caf-style) with cooperative scheduling
- Explicit lifecycle with optional hibernation
- Both statically and dynamically typed actors
- Hierarchical supervision (OneForOne, AllForOne)
- Pluggable service discovery: IServiceDiscovery interface with 4 backends (gossip, registrar, static, hybrid)
- Decentralized membership via SWIM gossip protocol — no single point of failure
- Production reliability roadmap is organized into data plane, control plane, and operations plane.
- Production backlog priority begins with explicit delivery semantics, bounded mailboxes, dead-letter queues, tracing correlation, health checks, and graceful shutdown.
- Typed memory regions with per-region back-pressure and observability
- Hibernation via serialization + madvise(MADV_PAGEOUT) to ZRAM for cold storage
- Actors are relocatable by ActorId, enabling slab compaction without dangling pointers
- Header-only library, C++20, no external dependencies (except OpenSSL for TLS)
- No exceptions (-fno-exceptions), no RTTI (-fno-rtti)
- constexpr ActorId constructor for constant initialization

## Current Progress

**Phase 0-16 Complete** (140 tests passing, 152 test source files)
- Phase 0: Local Message Delivery — actor spawn and local message routing
- Phase 1: ActorRef and Unified References — ActorRef as variant<Actor, ActorProxy>
- Phase 2: TCP Transport Implementation — kqueue/epoll event loop, TcpTransport, Connection
- Phase 3: Message Serialization — TypeTag enum, DefaultSerializer, Frame encode/decode
- Phase 4: Connection Pool and Handshake — TlsContext, TlsConnection, ConnectionPool, TLS handshake, AES-256-CBC encryption
- Phase 5: Service Discovery — UdpRegistrar, HostResolver, NodeRegistry, static routes, DNS resolution, RegistrarServer/RegistrarClient with TCP registration, heartbeat, failover
- Phase 6: Remote Actor Spawn — AsyncActor, ActorTypeRegistry, SpawnReceiver, spawn_remote()
- Scheduling Subsystem: ChaseLev deque, MultiPriorityWorkQueue, EDFQueue, A2WS, TimingWheel, CoroutineFramePool, HybridScheduler, WorkerThread, ActorState, CoroutineTask/CoroutinePromise, awaiters, MPSCMailbox

**Phase 8: Spawn Serialization Integration** ✅ Complete (2026-04-21)
- SpawnRequest/SpawnResponse integrated with TypeTag (SpawnRequestTag=5, SpawnResponseTag=6)
- SpawnMessageVariant (separate from main MessageVariant to avoid circular includes via spawn.hpp → serialization.hpp → abstract_actor.hpp)
- DefaultSerializer::encode_spawn()/decode_spawn() for spawn type serialization
- ActorSystem::spawn_remote_async() uses DefaultSerializer for request encoding
- ConnectionPool hybrid routing: SpawnResponse → spawn_handler, other RPC → rpc_handler
- SpawnReceiver sends SpawnResponse via transport with Frame context for reply routing
- AsyncActor gains message_id_ field for response correlation
- ActorTypeRegistry::spawn() updated to accept args and args_type parameters
- Remote child tracking added to SelfSupervisingActor (remote_children_, remote_child_addresses_, add_remote_child, etc.)
- ActorContext gains add_remote_child(ActorRef) method
- Integration test (test_spawn_integration) validates frame encoding and message correlation

**Phase 9-10 Complete** (Unified Message Passing + Zero-Copy Net)
- Unified message passing: proto_actor.hpp deleted, replaced by TypedMessage<T> + TypedEventBasedActor
- ErrorMsg TypeTag, reply_with_error(), current_sender_ capture for reply routing
- Zero-copy read path in reactor backends via std::span (5 copies → 2)
- Unified ReadStrategy abstraction for EpollBackend/KqueueBackend
- service_read_handler gated on pending op, accumulated reads
- MSG_PEEK infinite loop fix in EpollBackend recvfrom
- Reactor/Proactor separation: IReactorBackend, ReactorDispatcher, ProactorDispatcher

**Memory Management Phase ✅ Complete (2026-05-03)**
- 8 phases (M1-M8), 18 commits, 14 new memory tests
- Performance: bump alloc 25 ns/op, freelist recycle 32 ns/op, 1M ops in 650ms

**Next Steps (remaining items)**
- Production Reliability Plane remaining:
  - health/readiness/liveness endpoints
  - structured delivery results (explicit success/failure for sends)
  - reliable messaging (ACK/NACK, retry, dedup, durable outbox/inbox)
  - durable actor state (snapshot, event sourcing, recovery)
- Cluster control follow-up:
  - cluster failure model with quarantine/fencing
  - protocol/feature negotiation for rolling upgrades
  - sharding, placement, and rebalance protocol
- Production operations follow-up:
  - security architecture implementation (mTLS identity, authorization, audit)
  - authenticated admin API
  - dynamic config validation/diff/reload
  - chaos, soak, fuzz, and compatibility test lanes
- Proactor backend production hardening (GcdBackend, IoUringBackend)
- Full two-process integration test with TCP transport
- Wire up RPC trace context end-to-end (transport layer changes)
- HTTP ingress/egress trace propagation
- Typed RPC API (template call<Request, Response> with serialization)
- Tiny-block optimization for 32B size class (packed out-of-band metadata)
- Multi-node TOML topology (remote actor placement via dispatcher name)

**Source Reorganization**
- `include/hpactor/` — header-only library, organized by architectural group:
  - `actor/` — Actor base classes, behaviors, typed actors, spawn
  - `config/` — TOML topology config (topology_model, actor_factory, actor_factory_registry, toml_parser, binary_format, binary_serializer, binary_loader, actor_args)
  - `ref/` — Actor references (address, ref, proxy)
  - `net/` — Networking (event loop, TLS, connection pool, transports)
  - `supervision/` — Supervision strategies
  - `core/` — Core runtime (actor_system, mailbox, registry)
  - `sched/` — Scheduling subsystem (work_queue, edf_queue, a2ws, timing_wheel, coroutine_frame_pool)
  - `types/` — Type system (types, types_fwd, serialization)
  - `rpc/` — RPC channel (rpc_channel.hpp)
  - `mem/` — Memory management (alloc_header, size_class, freelist, segment_provider, slab_cache, thread_local_allocator, memory_region, memory_config, memory_tracker, telemetry_ring_buffer, hibernation_registry, hibernatable, guard_page, compaction, zram)
  - `cli/` — CLI subsystem (cli_actor, cli_config, cli_types, command_node, command_context, token, lexer, pager, output_formatter, pretty_formatter, json_formatter, tabular_formatter, commands/)
  - `metrics/` — Metrics subsystem (metrics_ring_buffer, metrics_event, metrics_config, metrics_registry, metrics_aggregator, metrics_formatter, metrics_actor)
  - `log/` — Structured logging (log_ring_buffer, logger, log_drain, log_sink, log_formatter, log_manager, log_config)
  - `tracing/` — Distributed tracing (trace_context, span, span_guard, trace_manager, trace_exporter, trace_config)
- `src/actor/` — actor_system.cpp, abstract_actor.cpp, actor_context.cpp, event_based_actor.cpp, local_actor.cpp, spawn_receiver.cpp
- `src/cli/` — cli_actor.cpp, lexer.cpp, command_node.cpp, pretty_formatter.cpp, json_formatter.cpp, tabular_formatter.cpp, pager.cpp
- `src/metrics/` — metrics_registry.cpp, metrics_aggregator.cpp, metrics_formatter.cpp, metrics_actor.cpp
- `src/log/` — log_manager.cpp, log_drain.cpp, log_sinks.cpp
- `src/tracing/` — trace_manager.cpp, trace_exporter.cpp
- `src/config/` — actor_factory_registry.cpp, toml_parser.cpp, binary_serializer.cpp, binary_loader.cpp
- `src/net/` — event_loop.cpp, acceptor.cpp, connection.cpp, tcp_transport.cpp, frame.cpp, tls_context.cpp, tls_connection.cpp, connection_pool.cpp, registrar.cpp
- `src/ref/` — actor_proxy.cpp, actor_ref.cpp
- `src/sched/` — scheduler.cpp, worker_thread.cpp, edf_queue.cpp, a2ws.cpp, timing_wheel.cpp, coroutine_frame_pool.cpp
- `src/spawn.cpp` — AsyncActor implementation
- `src/actor_type_registry.cpp` — ActorTypeRegistry implementation
- `src/core/serialization.cpp` — DefaultSerializer implementation
- `src/rpc/rpc_channel.cpp` — RpcChannel implementation
- `src/mem/` — segment_provider.cpp, slab_cache.cpp, thread_local_allocator.cpp, memory_config.cpp, memory_tracker.cpp, hibernation_manager.cpp, guard_page.cpp, compaction.cpp, zram.cpp
- `tools/toml-compiler/` — AOT compiler executable (compiler.cpp)
- Tests: `tests/{actor,cli,config,core,examples,log,mailbox,metrics,net,ref,rpc,sched,spawn,supervision,tracing,mem}/`

## Build Commands

```bash
# Configure and build
cmake -S . -B build -GNinja
ninja -C build

# Run tests
ctest --output-on-failure

# With sanitizers
cmake -DENABLE_TSAN=ON ..  # ThreadSanitizer
cmake -DENABLE_ASAN=ON ..  # AddressSanitizer (may show false positives in intrusive queue tests)

# Enable/disable examples (default ON)
cmake -DENABLE_EXAMPLES=OFF ..

# Memory management options
cmake -DENABLE_MEMORY_TRACKING=OFF ..  # Disable per-actor tracking (default ON)
cmake -DENABLE_MEMORY_DEBUG=ON ..     # Enable poisoning + canaries (default OFF)
```

## Known Issues

- ASAN may report false positives in `test_mailbox_awaiter` and `test_priority_scheduler` due to intrusive queue memory patterns. Tests pass cleanly with TSAN or without sanitizers.
- Proactor backend (`IoUringBackend`, `GcdBackend`) needs production hardening.
