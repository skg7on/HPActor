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
- Status: roadmap and backlog remain authoritative. Runtime foundations now implemented include scheduled messages, delivery-mode configuration, receiver deduplication, structured failure envelopes, bounded mailboxes, DLQ, distributed tracing, HTTP gateway, graceful shutdown, actor lifecycle, and actor quarantine. Durable outbox/inbox, ACK/NACK retry, cluster control, security, and operations-plane admin APIs remain design/backlog.

**Daemon Service & CLI Decoupling:** ✅ Design Complete (2026-06-14), Implementation Complete
- Issue: #284 — Actor System as a background process or service on Linux.
- Design spec: `docs/architecture/production/daemon-service-architecture-design.md` — systemd Type=notify, traditional daemon (double-fork), ProcessManager singleton, CliServerActor (UDS/TCP), standalone hpactor-cli binary, WatchdogActor, HealthHttpServer, SyslogSink.
- Implementation plan: `docs/superpowers/plans/2026-06-14-daemon-service-design.md` — 13 TDD tasks across 8 phases.
- New subsystem: `include/hpactor/process/` — ProcessManager, ProcessConfig, WatchdogActor, HealthHttpServer.
- New CLI components: `include/hpactor/cli/cli_session.hpp` (transport-agnostic command processor), `include/hpactor/cli/cli_server_actor.hpp` (socket-based CLI server), `include/hpactor/cli/cli_server_config.hpp`.
- New tools: `tools/hpactor-cli/` — standalone CLI client binary connecting via UDS/TCP.
- New log sink: `include/hpactor/log/syslog_sink.hpp` — POSIX syslog(3) sink for daemon mode.
- New config: `src/config/parsers/process_config_parser.cpp` — self-registering TOML parser for `[system.process]`.
- New deploy: `deploy/systemd/hpactor.service` — systemd unit file with Type=notify, WatchdogSec=10s, security hardening.
- New test files: test_process_manager (18 tests), test_cli_session (4 tests), test_cli_server_actor (2 tests), test_watchdog_actor (1 test), test_health_http (2 tests), test_syslog_sink (4 tests), test_daemon_integration (4 tests).
- Key invariants: All thread creation happens AFTER daemonization (ProcessManager::init() before ActorSystem construction). Foreground mode behavior unchanged — all 1845 existing tests pass. CliActor refactored to delegate command processing to CliSession without breaking existing CLI test suite (267 tests).
- Signals handled: SIGTERM/SIGINT → graceful shutdown, SIGHUP → config reload. signalfd on Linux, self-pipe fallback on other Unix.

**Failure Semantics & Delivery Foundation:** ✅ Complete (2026-05-20 to 2026-05-24)
- `FailureReason` enum (23 values in 10 semantic ranges) + `FailureSource` enum (12 subsystem origins) in `include/hpactor/types/failure_reason.hpp`.
- `FailureEnvelope` struct with full correlation metadata (actor_id, sender, receiver, message_id, trace, retryable, timestamp, source, detail) in `include/hpactor/types/failure_envelope.hpp`.
- `make_failure_envelope()` factory function with monotonic clock timestamp capture.
- `EnqueueResult::failure_reason()` — maps `EnqueueResultCode` → `FailureReason`.
- `error::failure_reason()` — maps `errors::` codes → `FailureReason`.
- Spawn errors and `DeadLetterReason` values map back to canonical failure reasons.
- `DeliveryMode` defines best-effort, observable best-effort, at-least-once, and durable-at-least-once policy intent.
- Receiver-side `DedupCache` suppresses duplicate `(source_node, source_actor, message_id)` tuples for tracked delivery.
- Delivery-deadline expiry maps to canonical `Expired` failure semantics.
- `try_deliver_local()` builds `FailureEnvelope` on failure paths, emits `kDeliveryFailure` metric event, and structured log warnings.
- CLI `/failure reasons` and `/failure summary` expose reason codes, retryability, and mapping status.
- Coverage includes `test_failure_reason`, `test_failure_envelope`, `test_delivery_mode`, `test_dedup_cache`, `test_is_expired`, spawn failure mapping, and delivery semantics integration tests.
- Design spec: `docs/architecture/production/structured-failure-envelope-design.md`. Implementation plan: `docs/superpowers/plans/2026-05-20-failure-envelope-phase1.md`.
- Delivery semantics design/plan: `docs/superpowers/specs/2026-05-24-msg001-delivery-semantics-design.md` and `docs/superpowers/plans/2026-05-24-msg001-delivery-semantics-impl.md`.

**Actor Quarantine & Circuit Breaker:** ✅ Complete (2026-05-23)
- `QuarantinePolicy` configures opt-in per-actor quarantine and circuit-breaker behavior.
- `CircuitBreakerTracker` tracks Closed/Open/HalfOpen state, cooldown timing, probe admission, trip count, and failure-rate EMA.
- `FailureRateTracker` computes failure and timeout rates over an observation window.
- `FailureReason::Quarantined` and `FailureReason::CircuitOpen` added to lifecycle failure range.
- Supervision can escalate repeatedly failing actors into quarantine instead of restart loops.
- Metrics include quarantine/unquarantine events.
- TOML `[system.quarantine]` parser provides system-level defaults with per-actor overrides.
- Design/plan: `docs/architecture/production/actor-quarantine-circuit-breaker-design.md` and `docs/superpowers/plans/2026-05-23-actor-quarantine-circuit-breaker-impl.md`.

**Deterministic Fault Injection Hooks:** ✅ Complete (2026-05-28, expanded 2026-05-30)
- `FaultController` — runtime opt-in controller owned by ActorSystem, disabled by default.
- `FaultSchedule` — pre-computed schedule of `(domain, tick, path, action, target)` entries with Builder API.
- `FaultPoint` / `FaultPointRegistry` — global trie with self-registration via static `FaultPointRegistrar` objects.
- `FaultDomain` — 14 per-subsystem tick counters (Mailbox, Transport, Scheduler, Allocator, Storage, Timer, Gossip, Config, Actor, and 5 more).
- `FaultAction` — 5 actions: Fail, Drop, Delay, Corrupt, Panic.
- `FAULT_INJECT(path)` macro — predictable cold branch via `HPACTOR_UNLIKELY`; `ENABLE_FAULT_INJECTION=OFF` eliminates all overhead.
- Hierarchical dot-separated path naming with wildcard scope matching (`hpactor.transport.*`).
- Expanded to 80 fault injection sites across 14 domains (up from 12 initial).
- FAULT_INJECT sites wired into `MPSCActorMailbox` (enqueue/dequeue), `TcpTransport` (try_send), and dozens of additional locations.
- `kFaultInjected` (26) metric event type.
- CLI `/fault status`, `/fault list`, `/fault clear` commands.
- `ENABLE_FAULT_INJECTION` CMake option (default ON).
- Seed replay determinism: same seed → same schedule → same failure.
- New source files: `src/fault/fault_controller.cpp`, `src/fault/fault_point_registry.cpp`, `src/fault/fault_points.cpp`, `src/fault/fault_schedule.cpp`.
- New test files: `test_fault_controller`, `test_fault_macro`, `test_fault_point`, `test_fault_schedule`, `test_fault_mailbox`, `test_fault_seed_replay`.
- Design spec: `docs/superpowers/specs/2026-05-28-fault-injection-hooks-design.md`.
- Implementation plan: `docs/superpowers/plans/2026-05-28-fault-injection-hooks-impl.md`.

**Mailbox Priority Lanes & MultiLaneQueue (MBX-005):** ✅ Complete (2026-05-30 to 2026-05-31)
- `MultiLaneQueue<T>` — lock-free multi-lane queue replacing `MPSCMailbox` as the core mailbox data structure. Multiple independent lanes with CAS-based enqueue/dequeue, dedicated system-message lane, priority-aware user lane routing, and `DropLowestPriority` overflow handler.
- Per-lane depth exposure in `MboxSnapshot` for observability.
- TOML `[system.mailbox]` config: `priority_aware` bool and `priority_levels` uint32_t, wired via `src/config/parsers/mailbox_config_parser.cpp`.
- Design spec: `docs/superpowers/specs/2026-05-30-priority-mailbox-lanes-design.md`.
- Implementation plan: `docs/superpowers/plans/2026-05-30-mbx-005-priority-lanes-impl.md`.
- Test files: `test_multi_lane_queue.cpp`, `test_priority_lanes.cpp`.

**Mailbox Overflow Handler Refactoring:** ✅ Complete (2026-05-28 to 2026-05-31)
- `IOverflowHandler` interface + `OverflowHandlerFactory` + `OverflowContext` replace monolithic overflow logic.
- Individual handlers: `RejectNewestHandler`, `DropNewestHandler`, `DropOldestHandler`, `DropLowestPriorityHandler`, `SpillToOverflowHandler`, `DeadLetterHandler`, `SignalOnlyHandler`.
- `PressureStateMachine` — tracks mailbox pressure (Low/High/Critical) with hysteresis.
- `ReservationManager` — atomic slot reservation for bounded admission.
- `BackpressureSignalGate` — coordinates backpressure signal emission (local, remote, both).
- `BackpressureSignalSerialization` — protobuf serialization for remote backpressure signals.
- `OverflowQueue` — bounded overflow queue with configurable policies.
- Test files: `test_overflow_handler_factory`, `test_overflow_handlers`, `test_pressure_state_machine`, `test_reservation_manager`, `test_backpressure_signal_gate`, `test_backpressure_signal_serialization`, `test_mailbox_overflow_queue`, `test_mpsc_relacy`.

**DLQ Handoff & CLI Commands (MBX-004):** ✅ Complete (2026-05-30)
- DLQ overflow integration — payload, trace context, and timestamp preserved in `DeadLetterRecord` during overflow.
- `DeadLetterQueue` API extended: `config()` accessor, `snapshot_records()` for atomically consistent snapshots, `try_pop_at()` for targeted record removal.
- `to_string()` for `DeadLetterReason` and `DeadLetterSource` values.
- `ActorSystem::dead_letter_queue()` accessor for programmatic DLQ access.
- CLI `/dlq list [actor_id]`, `/dlq show <index>`, `/dlq replay <index> [target]`, `/dlq export [actor_id]` commands via `src/cli/commands/dlq_commands.cpp`.
- `DeadLetterQueue` pointer wired into `OverflowContext` for future handler use.
- Test files: `test_dead_letter_queue.cpp`, `test_dlq_handoff.cpp` (system), `test_dlq_commands.cpp` (unit), `test_dlq_integration.cpp` (integration).
- Design spec and plan: `docs/superpowers/specs/2026-05-30-dlq-handoff-design.md` and `docs/superpowers/plans/2026-05-30-mbx-004-dlq-handoff-impl.md`.

**Scheduler Decoupling & Hardening:** ✅ Complete (2026-05-29 to 2026-05-30)
- `ActorExecutionEngine` — extracted coroutine execution logic from the scheduler into a standalone engine (`src/sched/actor_execution_engine.cpp`).
- `ActorReadyGate` — explicit ready-gate interface decoupling scheduler notification from execution.
- `IScheduler` / `IWorkPlacementScheduler` / `IWorkerNotification` — narrow interface segregation (`include/hpactor/sched/scheduler_interfaces.hpp`).
- `WorkPlacementScheduler` — extracted work placement strategy from HybridScheduler.
- `WorkerThread` replaces raw `std::thread` throughout `HybridScheduler` — fixes cross-thread `SlabCache` corruption from thread-local allocator propagation.
- `try_steal()` and exponential backoff wired into `WorkerThread::thread_loop`.
- Awaiter scheduler dependencies narrowed to minimal interfaces.
- Strict Doxygen on all new scheduler subsystem headers.
- Test files: `test_actor_ready_gate.cpp`, `test_work_placement_scheduler.cpp`.
- Design spec: `docs/superpowers/specs/2026-05-29-scheduler-decouple-design.md`.
- Implementation plan: `docs/superpowers/plans/2026-05-29-scheduler-decouple-impl.md`.

**ActorSystem Refactor Phase 0: Correctness Stabilization** ✅ Complete (2026-06-28)
- Consolidated actor names in `ActorDirectory`; `ActorSystem::ActorRegistry` is now a compatibility view backed by the directory.
- Preserved `DeadLetterQueue` object identity during topology configuration and added synchronized in-place `reconfigure()`.
- Aligned configured-spawn lifecycle, logger, metrics, and spawn-event behavior with template `spawn<T>()`.
- Added a synchronized `StreamRegistry` with atomic route removal and concurrent registration stress test.
- Verified full build and focused actor/config/mailbox/system test suites (105+ targeted tests, full CTest minus pre-existing gossip/NOT_BUILT).

**ActorSystem Refactor Phase 2: ActorRuntime + Unified Spawning** ✅ Complete (2026-06-28)
- Atomic directory publication (`ActorDirectory::publish()` with id+name atomicity).
- RTTI-free context binding (`bind_context()` + `activate_after_spawn()`) on AbstractActor/LocalActor.
- `SpawnSpec` + `ActorSpawner` with 16-step unified adoption state machine.
- Template `spawn<T>()` and `SpawnReceiver` routed through spawner.
- `ActorExecutionDependencies` struct + narrow `HybridScheduler` ctor with `exec_deps_`.
- `ActorRuntime` class implemented (ownership migration deferred to Phase 2b).
- Architecture test guards facade pointer-only invariant.
- 53 focused tests pass (unit + integration + system).

**ActorSystem Refactor Phase 3: MessagingRuntime Ownership** ✅ Complete (2026-06-30)
- `MessagingRuntime` class in `src/runtime/messaging_runtime.hpp/.cpp` is the sole
  owner of all seven messaging components: `DeadLetterQueue`, `DedupCache`,
  `msg::OutboundDeliveryTracker`, `mailbox::OutboundTracker`,
  `BackpressureCoordinator`, `DeliveryPipeline`, and `LocalDeliveryEngine`.
- All dependencies are fixed at construction — no late setters, no
  facade-capturing `std::function` callbacks on hot paths.
- Network control output uses narrow function-pointer/context ports
  (`ReliableAckPort`, `BackpressureWirePort`) bound to stable
  `NetworkRuntimeState` in `src/runtime/messaging_network_ports.hpp`.
- `DeliveryPipeline` receives concrete `ActorDirectory&`, DLQ, dedup, tracker,
  backpressure, metrics, and ACK port references at construction.
- `BackpressureCoordinator` has fixed directory, metrics, endpoint, and remote
  output dependencies; no production set_metrics_ring_buffer/set_transport.
- Facade delivery methods (`try_deliver_local`, `deliver_with_result`,
  `deliver_local`, `deliver_local_edf`, `try_deliver_local_fast`) route through
  `MessagingRuntime::try_deliver/deliver_with_result/try_deliver_fast`.
- Fast delivery classified by `FastDeliveryReason` enum:
  `StreamProtocol` (stream handlers), `CompatibilityExplicit` (public facade).
  `LocalDeliveryEngine` updated to use proper `try_push()` semantics.
- Reliable ACK/NACK in `deliver_remote()` routes through
  `MessagingRuntime::on_reliable_ack/on_reliable_nack`. Retry timer uses
  `MessagingRuntime::process_retries()`. `send_reliable_ack()` forwards to
  the fixed `ReliableAckPort`.
- `MessagingRuntime::reconfigure()` wraps DLQ reconfiguration preserving
  object identity. `load_topology()` routes through it.
- Remote ordinary delivery converges through `deliver_local()` →
  `MessagingRuntime::try_deliver()` — same full `DeliveryPipeline` as local.
- 15 architecture fitness checks enforce: no `ActorSystem*`/`Impl*` in messaging
  production code; no late-setter methods; no RTTI/exceptions in runtime files.
- Verification: 793 focused tests pass (unit/mailbox 302, unit/msg 44,
  integration/mailbox 18, integration/actor 237, integration/msg 27,
  integration/config 83, integration/sched 24, integration/tracing 25,
  integration/ref 17, architecture 16). ASan/TSan blocked by pre-existing
  macOS ARM platform issue (documented in CLAUDE.md).
- Design spec: `docs/superpowers/specs/2026-06-28-actor-system-phase3-messaging-runtime-design.md`.
- Implementation plan: `docs/superpowers/plans/2026-06-28-actor-system-phase3-messaging-runtime-implementation.md`.
- Explicit Phase 4/5 handoff: frame routing stays in `ActorSystem::deliver_remote()`;
  stream and network lifecycle ownership remain in future phases.
- Known gap: transport resend on retry is not implemented (characterized, not claimed).

**ActorSystem Refactor Phase 4: Frame & Stream Routing** ✅ Complete (2026-06-30)
- `FrameDecodeError` (7 values), `FrameDecodeLimits` (16 MiB), `FrameDecodeResult`,
  and `try_decode_wireframe()` in `include/hpactor/msg/frame.hpp`.
- `FrameDispatchCode`/`FrameDispatchResult` in `include/hpactor/net/frame_dispatch_result.hpp`.
- `WireFrameConnection::handle_read()` is now iterative (64 frames/turn max),
  enforces 16 MiB `max_inbound_frame_bytes` before allocation, and delivers
  canonical HPAC frame bytes (header + payload) matching the TLS path.
- `max_inbound_frame_bytes` added to `PoolConfig`; propagated from `TcpTransport`.
- `InboundFrameSink` (function-pointer + context, no virtual dispatch) in
  `include/hpactor/net/inbound_frame_sink.hpp`. Installed via
  `ConnectionPool::set_inbound_frame_sink()` and `TcpTransport::set_inbound_frame_sink()`.
  Unified sink has exclusive dispatch precedence; legacy handlers are fallback.
- `InboundFrameRouter` (`src/net/inbound_frame_router.hpp/.cpp`) is the sole
  classifier of valid HPActor envelopes. Oneof-first classification routes
  Data → MessagingRuntime/RpcChannel, Ack/Nack → typed reliable handlers,
  Batch → bounded per-entry full-policy delivery (1024-entry limit, partial
  aggregation), and all 5 Stream oneofs → StreamRuntime.
- Reliable flag disambiguation: dual-bit (AckRequested+AckResponse) → legacy ACK;
  AckResponse-only → legacy NACK; AckRequested-only → ordinary data metadata.
- `StreamRuntime` (`src/actor/stream_runtime.hpp/.cpp`) owns one peer-qualified
  (`StreamKey{EndPoint, uint64_t}`) bounded session map (`max_active_streams=4096`).
  Two-phase open (reserve Opening → spawn → commit Active) with rollback.
  Protocol handlers use correct wire TypeTags (StreamDataTag/StreamAckTag/
  StreamCloseTag/StreamWireErrorTag) and protobuf-aware TypedMessage construction.
- `StreamRuntimeSnapshot` in `include/hpactor/actor/stream_snapshot.hpp` for
  bounded CLI/admin visibility.
- Architecture tests enforce: no ActorSystem*/Impl captures in router/runtime;
  no recursive framing; no RTTI/exceptions in Phase 4 components.
- `src/` added to `hpactor_lib` PRIVATE include directories.
- Key non-goal: `ActorSystem::deliver_remote()` and stream facade methods are
  NOT yet converted to forwards (deferred to Phase 4b/5 integration step).
  InboundFrameRouter and StreamRuntime are built and tested independently.
- Verification: 997 tests pass (524 unit + 449 integration + 24 architecture).
- Design spec: `docs/superpowers/specs/2026-06-28-actor-system-phase4-frame-stream-routing-design.md`.
- Implementation plan: `docs/superpowers/plans/2026-06-28-actor-system-phase4-frame-stream-routing-implementation.md`.

**ActorSystem Refactor Phase 5: NetworkRuntime Extraction** ✅ Substantially Complete (2026-06-30)
- `NetworkRuntime` in `include/hpactor/runtime/network_runtime.hpp` + `src/runtime/network_runtime.cpp` is the sole owner of
  all network resources: `TcpTransport` (and its authoritative `EventLoop`), network
  thread, `IServiceDiscovery`/`UdpRegistrar`, `ActorLocationCache`, cache/retry
  maintenance timers, `RpcChannel`, `HttpClient`, and remote-spawn protocol integration.
- `NetworkRuntimeConfig` is an effective value object with no TOML parser or
  `ActorSystem::Config` back-reference. `NetworkRuntime::Dependencies` uses fixed
  function-pointer ports — no `std::function`, no facade-capturing lambdas.
- One authoritative event loop: `TcpTransport::loop()` replaces the old separate
  `network_loop_`. Discovery, HTTP client, cache purge, and retry timers all bind
  to this single loop. One network thread drives it.
- Construction is side-effect-free; `start()` uses 9-stage startup with reverse
  rollback on failure. `stop()` is idempotent and callback-quiescent.
- Self-stop from network thread returns `StopDeferred` (error code 10507);
  the Phase 6 coordinator completes the final join from a non-network thread.
- Port types: `NodeEventSink`, `OutboundRetryPort`, `RemoteSpawnPort`,
  `InboundFrameSinkPort`, `NetworkTelemetryPort` — all fixed-size, non-owning,
  function-pointer + context void*.
- Adapter functions (`reliable_ack_adapter`, `backpressure_wire_adapter`) use
  `NetworkRuntime*` as context, not `NetworkRuntimeState*` or `ActorSystem*`.
- Facade accessors (`event_loop()`, `transport()`, `registrar()`, `rpc_channel()`,
  `http_client()`, `get_transport_for()`) forward through `NetworkRuntime` with
  legacy fallbacks for the old `NetworkRuntimeState` fields (Phase 8 cleanup).
- `NetworkSnapshot` public type in `include/hpactor/net/network_snapshot.hpp` for
  bounded CLI/admin observability.
- 11 architecture checks enforce: no `ActorSystem*`/`Impl*` captures in network
  runtime files; no late dependency setters; no second `EventLoop` creation;
  no `std::function` in network runtime; no RTTI/exceptions.
- New files: `include/hpactor/runtime/network_runtime.hpp`, `src/runtime/network_runtime.cpp`,
  `include/hpactor/runtime/network_runtime_callbacks.hpp`, `include/hpactor/net/network_snapshot.hpp`.
- Test files: `tests/unit/runtime/test_network_runtime_lifecycle.cpp` (14 tests).
  All 1,788 focused tests pass (14 runtime + 199 unit/net + 195 integration/net
  + 17 rpc + 20 spawn + 302 unit/mailbox + 275 unit/actor + 237 integration/actor
  + 44 unit/msg + 83 integration/config + 154 unit/sched + 64 unit/core +
  27 unit/config + 44 unit/fault + 47 unit/ref + 24 integration/sched +
  17 integration/ref + 25 integration/tracing). 42 architecture tests pass.
- Known gaps: SpawnReceiver still manually constructed in ActorSystem constructor
  (Phase 6 full integration); legacy `NetworkRuntimeState` fields retained as
  fallbacks (Phase 8 removal); transport resend on retry not implemented;
  ASan/TSan blocked by pre-existing macOS ARM platform issue.
- Design spec: `docs/superpowers/specs/2026-06-28-actor-system-phase5-network-runtime-design.md`.
- Implementation plan: `docs/superpowers/plans/2026-06-28-actor-system-phase5-network-runtime-implementation.md`.
- Explicit Phase 6 handoff: `NetworkRuntime` is the network owner; Phase 6
  `RuntimeCoordinator` calls `network_->start()`/`network_->stop()` in proper
  startup/shutdown order with immutable `RuntimeBlueprint`.

**ActorSystem Refactor Phase 6: Runtime Blueprint and Lifecycle** ✅ Complete (2026-06-30)
- `RuntimeBlueprint` — immutable validated startup config with FNV-1a fingerprint.
- `RuntimeBlueprintBuilder` — Config→blueprint with validation, no side effects.
- `BlueprintNetworkConfig`, `ActorRuntimeConfig`, `MessagingRuntimeConfig`,
  `StreamRuntimeConfig`, `ConfiguredActorSpec` — component config value types.
- `ReloadClass` (Live/RestartRequired/Immutable), `ConfigPathId`,
  `ConfigFieldDescriptor`, `ConfigFieldRegistry`, `ReloadReport` —
  in `include/hpactor/config/reload_report.hpp`.
- `RuntimeBuilder` — composition root; constructs stopped `ActorSystem`
  from blueprint without starting threads/listeners/actors.
- `RuntimeCoordinator` — non-owning lifecycle state machine (Built→Starting→
  Running→Draining→Stopping→Stopped, with Failed rollback).
- `RuntimeLifecycleStage`, `LifecycleAction`, `RuntimeLifecycleSnapshot` —
  stage descriptors with start/rollback/destroy actions.
- `register_runtime_startup_stages()` — wires real component stages (scheduler,
  fault controller) into coordinator.
- `ActorSystem::create(config)` and `create(config, topology)` — preferred
  result-returning factory. Legacy constructor preserved.
- `RuntimeCoordinator::shutdown()` — unified drain/stop path, idempotent.
- `RuntimeBlueprintBuilder::diff()` — fingerprint-based reload classification.
- Architecture enforcement: 10 Phase 6 CTest checks (no RTTI/exceptions in
  lifecycle files, no std::function in coordinator).
- New files: `src/runtime/runtime_blueprint.hpp/.cpp`,
  `src/runtime/runtime_blueprint_builder.hpp/.cpp`,
  `src/runtime/runtime_builder.hpp/.cpp`,
  `src/runtime/runtime_coordinator.hpp/.cpp`,
  `src/runtime/runtime_startup.hpp/.cpp`,
  `include/hpactor/config/reload_report.hpp`.
- Test files: `test_runtime_lifecycle_boundaries.cpp` (13),
  `test_runtime_blueprint.cpp` (13), `test_reload_classification.cpp` (12),
  `test_runtime_builder.cpp` (6), `test_runtime_coordinator.cpp` (15),
  `test_coordinated_startup.cpp` (7), `test_unified_shutdown.cpp` (9),
  `test_actor_system_factory.cpp` (5).
- Verification: 94 runtime unit tests pass, 10/10 architecture checks pass.
- Known gaps: full blueprint constructor fix (ordering with
  ActorExecutionDependencies), network-enabled blueprint path (needs full
  transport config), topology actor deployment transaction (separately reported).

**Python Binding Phase 1A Native Foundation** ✅ Complete (2026-07-07)
- `hpactor_python_native` CMake library — built only when `ENABLE_PYTHON_BINDINGS=ON`; depends on `hpactor_lib` only; no CPython link.
- `PythonRuntimeQueues` — three independent bounded lock-free MPSC rings (dispatch, command, completion) with explicit capacity, rejection accounting, `drain_up_to()` with callback, and `DRAIN_UP_TO_MAX` budget guard; base class `MpscRingBuffer<T>` shared with metrics/logging subsystems.
- `NativeNotifier` — non-blocking eventfd (Linux) / pipe (macOS) notifier, each owning a read/write fd pair; write-side closed at shutdown, no blocking or thread primitives.
- `PythonRuntime` — lifecycle state machine (Created→Running→Draining→Stopping→Stopped/Failed); owns three queues + two notifiers; `PythonActorLease` with monotonic generation; admission controlled by state; dispatch depth and rejection counters on `PythonRuntimeSnapshot`.
- `PythonBridgeActor` — `EventBasedActor` subclass bound via `PythonActorLease`; converts `TypedMessage` envelopes to `PythonDispatchEnvelope` with full metadata (sender, message_id, priority, deadline, trace); reliable messages ACK/NACK on transfer; dispatch queue acts as the Python event-loop's inbox.
- `PythonGatewayActor` — budgeted command executor with per-turn `max_commands_per_turn` limit; self-wakes via `kPythonWakeupTag` system tag re-queue when commands remain; fixed function-pointer `PythonCommandExecutorPort` executes commands with completions pushed to the completion queue.
- `PythonGatewayWakeAdapter` — bridges the runtime's `GatewayWakePort` callback (called from Python thread) to the gateway actor via `deliver_with_result()`.
- `PythonPorts` — `GatewayWakePort`, `PythonCommandExecutorPort`, `PythonCommandExecution` — all fixed-size function-pointer ports (no `std::function`, no exceptions).
- Tests: 7 unit test files (contracts, queues, runtime, notifier, bridge, gateway, stress), 1 integration test (end-to-end native workflow), architecture fitness checks (no Python.h/PyObject/RTTI/exceptions in binding files; no Python.h in core).
- No public Python actor API or distributable wheel exists yet; the manual's
  language-binding limitation remains accurate until Phase 1D.
- Design spec: `docs/superpowers/specs/2026-07-03-python-language-binding-design.md`.
- Implementation plan: `docs/superpowers/plans/2026-07-03-python-binding-phase1a.md`.

**Python Binding Phase 1B Actor API** ✅ Complete (2026-07-08)
- `CompletionPort<T>` — fixed function-pointer + context + keepalive completion port for move-only values; no `std::function`, no exceptions. Headers in `include/hpactor/msg/completion_port.hpp`.
- `RequestHandle<T>::on_complete(CompletionPort)` and `DeliveryReceipt::on_complete(CompletionPort)` — exactly-once, mutex-safe fixed-port completions. Both forms reject double registration.
- `python_binding_internal.proto` — process-local `PbPythonActorCommand` (23 fields) and `PbPythonActorFailed` (7 fields). Codec versioned with bounded-field validation (payload ≤ 16 MiB, detail ≤ 16 KiB, name ≤ 255 bytes).
- `PythonCommand`/`PythonCompletion` extended with Phase 1B fields: `reply_to`, `message_id`, `ask_message_id`, `schedule_handle`, `detail`, `actor_name`, `delivery_mode`, `no_drop`, `emit_backpressure`, `FailureSource`, `error_code`, `delivery_status`, `retry_after_ns`.
- `PythonCommandKind::CancelAsk` appended; `PythonCompletionKind::ScheduleResult`/`ActorStopped`/`ActorFailed` added.
- `PythonCommandCodec` — deterministic protobuf encode/decode with validation of version, command kind, tag range (0x1000..0x00FFFFFF), and bounded fields.
- Protected tags `0xF0`–`0xF3` rejected from ordinary and batch remote ingress in `InboundFrameRouter` (`is_python_binding_control_tag()` helper, `FrameDispatchCode::InvalidControlPayload`).
- `PythonCommandRouter` — converts queue commands to protected F1/F2 system-lane messages and delivers to the originating bridge actor. Installed as the gateway's `PythonCommandExecutorPort`. Validates origin ActorId and generation before delivery.
- `ActorContext::ask_raw(TypeTag)` — typed overload for request-response with explicit `TypeTag`, stamps generated ask message ID.
- `PythonNativeSystem` — value-only ownership facade (owns `ActorSystem`, `PythonRuntime`, `PythonCommandRouter`, gateway wake adapter, application bridge). Lifecycle: `create` → `start` → (use) → `begin_draining` → `stop`. Methods: `spawn_bridge`, `stop_bridge`, `register_name`, `resolve_name`, `application_origin`, `submit`, `dispatch_read_fd`, `completion_read_fd`, `drain_dispatch`, `drain_completions`, `snapshot`.
- `_hpactor` CPython limited-API extension scaffold — `Py_LIMITED_API=0x030B0000`, heap type `NativeSystem` wrapping `PythonNativeSystem*`. Methods: `start`, `stop`, `application_origin`, `dispatch_fd`, `completion_fd`. Python.h confined to `bindings/python/native/src/python_capi/`.
- Pure-Python `hpactor` package: `_address` (frozen dataclasses), `_errors` (9 typed exceptions), `_delivery` (6 enums + `DeliveryOptions`/`DeliveryResult`/`DeliveryReceipt`), `_messages` (freezeable `MessageRegistry` with deterministic serialization), `_behavior` (immutable validated handler tables), `_actor` (`@actor` decorator + lifecycle hooks), `_context` (handler-scoped `ActorContext`), `_runtime` (bounded `_DispatchCoordinator`, `_ActorRunner` with per-actor FIFO, `_TokenRegistry`, dedicated `_RuntimeThread`), `_system` (`ActorSystem` async context manager with `spawn`/`send`/`ask`).
- Tests: 7 C++ unit test files (command codec round-trip, tag detection, ask_raw overload signature, plus existing Phase 1A tests); 4 Python unit test files (address/delivery, registry, behavior/actor, actor system) — 22 Python tests + 12+ C++ tests passing. 183 architecture checks passing.
- Key invariants: native queues/callbacks/facades never store Python objects. `Python.h`/`PyObject` confined to `python_capi/` directory. CAPI files contain no RTTI/exceptions/`std::function`. No RTTI/exceptions in any native binding production code.
- The Phase 1B API is a build-tree development surface. ABI3 wheel production, repair, installation docs, and supported distribution begin in Phase 1D; the developer manual's "no official bindings" limitation remains accurate.
- Design spec: `docs/superpowers/specs/2026-07-03-python-language-binding-design.md`.
- Implementation plan: `docs/superpowers/plans/2026-07-05-python-binding-phase1b-actor-api.md`.

**Python Binding Phase 1C Reliability and Operations** ✅ Complete (2026-07-08)
- `config::PythonBindingConfig` — dependency-free validated config for `[system.python]` TOML table (10 fields with power-of-two capacities, drain budgets, loop lag, shutdown timeout). Self-registering parser at `kOrder=105` via `TomlSystemParserRegistration`.
- `PythonDispatchKind` enum (Message=0, LinkedExit=1, MonitorDown=2, Restart=3) and bounded `PythonFailureMetadata` (FailureReason, FailureSource::LanguageBinding, error_code, exception_type, detail, traceback).
- `PythonActorSnapshot` — point-in-time CLI-inspectable state (generation, handled, failures, restarts, cancellations, pending_turns, active_turn, quarantined, actor_type).
- `PythonRuntime` heartbeat and counter API: `record_heartbeat(now_ns)`, `record_dispatch_rejected()`, `record_command_rejected()`, `record_handler_exception()`, `record_handler_cancelled()`, `record_stale_completion()`. `PythonRuntimeSnapshot` extended with `dispatch_rejected`, `command_rejected`, `handler_exceptions`, `handler_cancelled`, `stale_completions`, `last_heartbeat_ns`, `loop_lag_ns`, `ready`.
- `PythonReliabilityController` — bounded table keyed by ActorId with generation, `PythonSupervisionConfig` (max_restarts, restart_window_ms, quarantine_on_exhaustion), restart budget window tracking. Returns `PythonFailureDirective` (Restart/Stop/Escalate/Quarantine). Fixed `PythonReliabilityPort` function-pointer callbacks.
- `PythonBridgeActor` now inherits both `EventBasedActor` and `LifecycleActor` with `as_lifecycle()` override and full lifecycle hooks: `on_drain()`, `on_stop()`, `on_deactivate()`, `on_fail(error)`, `on_restart()`, `on_quarantined(QuarantineReason)`. Restart allocates replacement generation through `PythonRuntime::reserve_actor()`, enqueues `PythonDispatchKind::Restart` dispatch.
- `PythonObservability` — stub surface for 11 metric families, bounded structured logs, and `begin_handler_span`/`finish_handler_span` span token API.
- `PythonInspectionService` — bounded asynchronous inspection with `inspect()`/`complete()`/`cancel_all()`; `PythonInspectResult` with detail_json bounded at 16 KiB.
- `PythonRuntimeHealthCheck` — snapshot-only readiness check returning Healthy when Running with fresh heartbeat; queue pressure alone does not make the node unready.
- `PythonShutdownAdapter` — 10-step shutdown coordinator with `handler_shutdown_timeout_ms`, `python_objects_quiesced` flag for late-callback fencing.
- `PythonDispatchEnvelope` extended with `PythonDispatchKind kind` and bounded `PythonFailureMetadata failure` for non-message dispatch records.
- `PythonNativeSystem` updated with `PythonReliabilityController reliability_` member; spawn calls pass `PythonSupervisionConfig`.
- Build: `python_binding_config.cpp` and `parsers/python_binding_config_parser.cpp` added to `hpactor_lib`; `python_reliability.cpp`, `python_observability.cpp`, `python_inspection.cpp`, `python_health_check.cpp` added to `hpactor_python_native`.
- CAPI build fixes: `PyModuleDef` m_slots initializer, `PyType_GetSlot` for tp_free, `reinterpret_cast` for type slots, forward-declaration-based `python_health_check.hpp` and `python_shutdown_adapter.hpp`.
- Tests: `test_python_binding_config.cpp` (19 tests: config defaults, validation bounds, append-only enum values, Phase 1C contract invariants). All 206 Python binding tests pass (up from 183).
- The Phase 1C bridge is a build-tree development surface. ABI3 wheel production, platform repair, clean-environment installation, release documentation, and supported distribution begin in Phase 1D; the developer manual's "no official bindings" limitation remains accurate.
- Design spec: `docs/superpowers/specs/2026-07-03-python-language-binding-design.md`.
- Implementation plan: `docs/superpowers/plans/2026-07-05-python-binding-phase1c-reliability-operations.md`.

**Python Binding pybind11 Backend** ✅ Complete (2026-07-10)
- Replaced raw CPython limited C API (`bindings/python/native/src/python_capi/`) with pybind11 backbone (`bindings/python/native/src/python_pybind11/`).
- **pybind11:** v2.13.6 vendored under `third_party/pybind11/include/` (36 header files, SHA256 verified).
- **ABI3:** deferred — `Py_LIMITED_API` / `PYBIND11_USE_LIMITED_API` not enabled; pybind11 2.13.6 has incomplete limited-API guards. Full Python API used for now; ABI3 compliance to be verified by `abi3audit` follow-up.

**Python Binding Phase 1D Packaging and Release** ✅ Complete (2026-07-11)
- Package metadata: pyproject.toml with scikit-build-core, cp311 ABI3 wheel config, setuptools-scm dynamic versioning, protobuf>=7.35.0,<8 dependency.
- ``hpactor.__version__`` via ``importlib.metadata`` with ``"0+unknown"`` fallback for build-tree usage.
- CMake wheel install layout: ``HPACTOR_PYTHON_WHEEL_BUILD`` option, ``cmake/python_wheel_install.cmake`` with ``python-wheel`` component, private runtime libs in ``hpactor/.libs/``, relative RPATH ($ORIGIN/.libs on Linux, @loader_path/.libs on macOS).
- Hermetic dependency lock: ``native-deps.lock.json`` (OpenSSL 3.5.5, Abseil 20260107.1, protobuf 35.0, all SHA256 pinned), ``fetch_source.py`` with content-addressed cache, ``build_native_deps.py`` for platform-correct static PIC builds.
- Wheel audit: ``dependency-policy.json`` with per-platform architecture rules, ``verify_wheel.py`` for ABI3 tag, content, metadata, and RPATH checks.
- CI wheel matrix: ``python-wheels.yml`` with 4 targets (manylinux_2_28 x86_64/aarch64, macosx_12_0 x86_64/arm64), cibuildwheel, auditwheel/delocate repair, acceptance gate requiring exactly 4 wheels.
- Trusted publishing: ``python-publish.yml`` with tag/version consistency gates, OIDC trusted publishing (id-token: write), protected PyPI/TestPyPI environments, no API tokens.
- Wheel smoke tests: import quiescent (no threads/fds), metadata validation, smoke requirements.
- Examples: echo.py (first actor), operations.py (fire-and-forget + shutdown), all use explicit protobuf TypeTags.
- Python manual: updated ``limitations.rst`` with Phase 1D status, 4 supported wheel targets, alpha stability.
- Performance gates: ``bench_actor_runtime.py`` (throughput, p50/p95/p99), ``compare_python_binding_perf.py`` with same-runner fingerprint enforcement and 20% regression threshold.
- Remaining: Phase 1E declarative topology, Phase 2 external SDK, pybind11 limited-API fix (abi3audit follow-up).
- Package tests: 19 packaging tests passing (metadata 4, cmake layout 3, lock manifest 5, binary policy 7, CI matrix 3).
- Design spec: ``docs/superpowers/specs/2026-07-03-python-language-binding-design.md``.
- Implementation plan: ``docs/superpowers/plans/2026-07-05-python-binding-phase1d-packaging-release.md``.
- **Exception boundary:** `python_pybind11/` TUs compile with `-fexceptions -frtti`; all other binding TUs remain `-fno-exceptions -fno-rtti`. Architecture scans updated to exclude `python_pybind11/` from the strict no-exception/RTTI check and add a pybind11-specific scan (forbids `dynamic_cast` and `std::function` only).
- **`NativeSystemObject`:** noexcept wrapper around `PythonNativeSystem*` with `guard()` template that catches `pybind11::error_already_set` and converts to sentinel values. Exposes all 23 methods: lifecycle (start/stop/drain), actor management (spawn/stop/resolve), messaging (submit/drain_dispatch/drain_completions), observability (snapshot/fds), topology stubs (Phase 1E).
- **Wire format:** dict-based dispatch/completion/command (named keys), replacing positional tuples.
- **Python package:** unchanged — 10 pure-Python modules require zero changes.
- **Tests:** 1 pybind11 availability test + 6 NativeSystemObject tests (construction, lifecycle, origin, drain, snapshot, guard error conversion). All 42 existing C++ tests pass. 209/209 CTest PythonBinding tests pass (zero regressions).
- Design spec: `docs/superpowers/specs/2026-07-10-pybind11-backend-design.md`.
- Implementation plan: `docs/superpowers/plans/2026-07-10-pybind11-backend-implementation.md`.
- Phase 1D plan refined: `docs/superpowers/plans/2026-07-05-python-binding-phase1d-packaging-release.md` (pybind11 vendoring, `pybind11_add_module`, exception boundary, binary audit updates).
- Umbrella spec updated: `docs/superpowers/specs/2026-07-03-python-language-binding-design.md` (Section 1, Section 20).

**ActorSystem Refactor Phase 1: Runtime Ownership Shell** ✅ Phase 1a Complete (2026-06-28)
- Introduced private `ActorSystem::Impl` in `src/runtime/` with named state groups:
  `CoreRuntimeState`, `ActorServiceState`, `MessagingRuntimeState`,
  `NetworkRuntimeState`, `OperationsRuntimeState`, `ClusterRuntimeState`.
- Added `adopt_preconstructed_actor()` spawn bridge — template `spawn<T>()`
  constructs `T` and delegates out-of-line, enabling future field migration.
- Added architecture fitness tests: public-header compilation check,
  `assert_file_excludes.cmake` reusable field-exclusion script.
- `src/runtime/actor_system_impl.hpp` is private to `hpactor_lib`.
- State groups are defined but fields remain in the facade header for now;
  field migration to Impl is deferred to Phase 1b to keep reviews focused.
- Next: move facade fields into named Impl state groups and convert inline
  accessors to out-of-line.

**EdgeOps Telemetry Platform:** ✅ Complete (2026-05-31)

**Ask Timeout Standardization (ACT-007):** ✅ Complete (2026-06-06)
- `RequestTimeout` type — explicit timeout specification (Default, Infinite, explicit Duration, Immediate) with `use_default()` and `never()` named constructors in `include/hpactor/types/request_timeout.hpp`.
- `RequestHandle<T>` — move-only shared-state future for ask responses with `get()`, `ready()`, `cancel()`, `message_id()`, and `deadline()` in `include/hpactor/types/request_handle.hpp`.
- `AskManager` subsystem — owned by `ActorSystem`, tracks in-flight ask requests, correlates responses by `MessageId`, schedules timeout timers, and resolves handles on reply or timeout in `include/hpactor/actor/ask_manager.hpp` and `src/actor/ask_manager.cpp`.
- System config fields: `default_ask_timeout_ms` (default 5000ms) and `default_ask_max_retries` (default 3) in `include/hpactor/config/system_fields.def`.
- Self-registering TOML parser for `[system.ask]` in `src/config/parsers/` — parses `ask.default_timeout_ms` and `ask.max_retries`.
- `ActorContext::ask()` and `ActorContext::ask_raw()` — message sending with response correlation via `AskManager`, returns `RequestHandle<StreamBuffer>` or typed `RequestHandle<T>`.
- `EventBasedActor::receive()` sets `current_ask_message_id` from incoming messages so `reply()` automatically correlates responses.
- `RpcChannel` deadline enforcement in `on_timeout()` — emits `FailureEnvelope` with `DeadLetterReason::AskTimeout`, supports configurable `max_retries` from system config.
- `RpcFuture<T>::ready()` non-blocking readiness check.
- `SpawnReceiver` wiring fix — validates spawn integration with ask subsystem.
- Metric event types for ask lifecycle: `kAskRegistered`, `kAskResolved`, `kAskTimeout`.
- `DeadLetterReason::AskTimeout` (38) — ask deadline expiry recorded in DLQ.
- CLI `/ask pending`, `/ask cancel <msg_id>`, `/ask stats` commands registered in `src/cli/commands/ask_commands.cpp` (forward-looking stubs, AskManager inspection API not yet exposed).
- Unit tests: `test_request_timeout` (8 tests), `test_request_handle` (12 tests), `test_ask_manager` (10 tests).
- Integration tests: `test_ask_local` (3 tests — AskManager creation, unready handle, on_response resolution), `test_rpc_deadline` (1 test — configurable max_retries).
- Design spec: `docs/superpowers/specs/2026-06-01-act-007-ask-timeout-policy.md`.
- Implementation plan: `docs/superpowers/plans/2026-06-01-act-007-ask-timeout-policy.md`.
- New complex demo app (`apps/edgeops_telemetry/`) validating actor lifecycle, message routing, rollup aggregation, alert rule evaluation, backpressure handling, DLQ evidence collection, operator query workflows, and same-host role-mode runbook.
- EdgeOps-specific message types, alert rules engine, and rollup aggregator.
- Order platform relocated from `examples/` to `apps/order_platform/`.
- Full design spec under `docs/app/edgeops-telemetry-platform-design.md`.
- App design docs consolidated under `docs/app/`.
- Test files: `test_edgeops_messages.cpp`, `test_system_edgeops_telemetry.cpp`.

**CLI Test Coverage Expansion:** ✅ Complete (2026-05-31)
- 75 new CLI tests across 6 files targeting low-coverage subsystems.
- New test files: `test_actor_commands.cpp`, `test_command_utils.cpp`, `test_dlq_commands.cpp`, `test_failure_commands.cpp`, `test_fault_commands.cpp`, `test_help_quit_commands.cpp`, `test_misc_commands.cpp`, `test_system_commands.cpp`.
- Pure completion/hint logic extracted from line editor callbacks into `cli_test_helpers.hpp` for testability.
- CLI test simplification: hoisted `find_cmd` helper, fixed timestamp handling, removed dead code.

**Build & Polish:** ✅ Complete (2026-05-29 to 2026-05-31)
- Clang-tidy made optional: `ENABLE_CLANG_TIDY` CMake option (default OFF).
- 7 mailbox code review findings addressed.
- EdgeOps app simplified after review: enum status codes, static rule tables, move semantics.
- Complete Apache 2.0 license headers on all new files.
- Clang-tidy DeMorgan false positive suppressed in FAULT_INJECT macro usage.

**Shared ADT Extraction:** ✅ Complete (2026-05-18 to 2026-05-20)
- `Id<Tag, T>` template plus tag types back opaque identifiers such as ActorId, MessageId, AlarmHandle, and timer IDs.
- `NodeIdentity` deduplicates node/member identity fields across discovery and registrar code.
- `adt::MpscRingBuffer` extracted as a shared per-slot publish/sequence ring buffer used by metrics/logging/telemetry-style paths.
- `DispatchPolicy` enum deduplicated into the shared type layer.
- Config schema fields for system/mailbox/dispatcher settings moved to X-macro tables.
- Design/plan: `docs/superpowers/specs/2026-05-18-shared-adt-extraction-design.md` and `docs/superpowers/plans/2026-05-18-shared-adt-extraction-plan.md`.

**Memory Region Accounting & Pressure Admission:** ✅ Complete (2026-05-23)
- `MemoryRegionRegistry` tracks typed region stats, hard limits, high-water pressure state, corruption events, and rejected allocation counts.
- Alloc headers preserve allocation region and fallback provenance.
- SegmentProvider, SlabCache, ThreadLocalAllocator, and std allocator paths participate in region accounting.
- ActorSystem memory config exposes region limits and admission hooks.
- Coverage includes `test_memory_region_accounting`, expanded alloc header tests, telemetry ring buffer tests, and allocator pressure paths.
- Plan: `docs/superpowers/plans/2026-05-22-memory-region-accounting-pressure-impl.md`.

**Test Reorganization & Coverage Infrastructure:** ✅ Complete (2026-05-22 to 2026-05-24)
- Google Test v1.14.0 vendored under `third_party/googletest/`.
- Tests reorganized into `tests/unit`, `tests/integration`, and `tests/system` with tier-level CMake files and `gtest_discover_tests`.
- Current tree contains 32 GTest binaries, 219 test source files, and 1411 source-level `TEST`/`TEST_F`/`TEST_P` definitions.
- Added 51 network subsystem system/integration tests and 66 additional low-coverage tests across CLI, config, ref, supervision, net, and system paths.
- Coverage workflow now has an `ENABLE_COVERAGE` CMake option and gcov branch coverage support.
- Design/plan: `docs/superpowers/specs/2026-05-21-test-reorganization-design.md`, `docs/superpowers/plans/2026-05-22-test-reorganization-impl.md`, and `docs/superpowers/specs/2026-05-24-coverage-cmake-option-design.md`.

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
- Typed memory regions: kActor, kMessage, kCoroutine, kNetwork, kInternal, kHibernate, with per-region accounting and pressure admission.
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
- Simple API examples are built via `ENABLE_EXAMPLES` CMake option (default ON)

**Complex Applications:** ✅ Complete / Expanding
- `apps/order_platform/13_order_platform.cpp` — multi-role order-processing app
  demonstrating actors, scheduling, bounded mailboxes, DLQ, CLI/ops probes,
  HTTP gateway, remote spawn, and query workflows.
- `apps/edgeops_telemetry/14_edgeops_telemetry.cpp` — IoT edge telemetry app
  validating actor lifetime, message routing, rollups, alerts, backpressure,
  DLQ evidence, operator queries, and same-host role-mode runbook.
- Complex apps are built via `ENABLE_APPS` CMake option (default ON).

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

**Full-Featured App:** ✅ Complete (2026-05-16, PR #102)
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

**Tests:** ✅ 32 GTest binaries configured (219 test source files across 3 tiers)
- Current tree contains 1411 source-level `TEST`/`TEST_F`/`TEST_P` definitions.
- Three-tier structure using Google Test framework (vendored in `third_party/googletest/`).
- **unit** (120 files): actor (6), adt (1), cli (16), config (1), core (4), fault (4), log (6), mailbox (23), mem (16), net (17), ref (1), sched (16), spawn (1), supervision (2), tracing (5).
- **integration** (82 files): actor (28), cli (3), config (7), fault (2), log (1), mailbox (2), metrics (3), net (11), ref (4), rpc (1), sched (4), spawn (5), supervision (4), tracing (7).
- **system** (17 files): edgeops (2), examples (1), plus cross-subsystem backpressure, DLQ handoff, graceful shutdown, loopback network, observability, order platform, registrar, runtime workflow, supervision, TCP transport, topology bootstrap, and UDP registrar tests.

**Documentation:** ✅ Complete
- Architecture: `docs/architecture/production/production-reliability-plane.md` (24x7 production reliability roadmap)
- Architecture: `docs/architecture/production/architecture-requirement-backlog.md` (summary production requirement backlog)
- Architecture: `docs/architecture/production/feature-gap-refined-requirement-backlog.md` (detailed feature-gap requirement backlog)
- Architecture: `docs/architecture/mailbox/actor-concurrency-and-lockfree-mailbox-rules.md` (normative actor concurrency, MPSC mailbox, scheduler readiness, and concurrency test rules)
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
- Spec: `docs/architecture/mem/memory-management-architecture-design.md` (memory management)
- Spec: `docs/architecture/config/actor-toml-config-core-concept.md` (TOML config philosophy)
- Spec: `docs/architecture/config/actor-toml-config-architecture.md` (TOML config detailed spec)
- Plan: `docs/superpowers/plans/2026-05-03-toml-config-topology-impl.md` (TOML config implementation)
- Plan: `docs/superpowers/plans/2026-05-03-memory-management-impl.md`
- Architecture: `docs/architecture/metrics/actor-metrics-design.md` (metrics core concept)
- Architecture: `docs/architecture/cli/cli-interactive-design.md` (CLI interactive core concept)
- Spec: `docs/superpowers/specs/2026-05-04-actor-metrics-design.md` (metrics detailed spec)
- Spec: `docs/superpowers/specs/2026-05-05-actor-cli-interactive-design.md` (CLI interactive detailed spec)
- Plan: `docs/superpowers/plans/2026-05-04-actor-metrics-impl.md` (metrics implementation plan)
- Plan: `docs/superpowers/plans/2026-05-05-actor-cli-interactive-impl.md` (CLI interactive implementation plan)
- Spec: `docs/superpowers/specs/2026-05-18-shared-adt-extraction-design.md` (shared ADT extraction)
- Plan: `docs/superpowers/plans/2026-05-18-shared-adt-extraction-plan.md` (shared ADT extraction implementation)
- Architecture: `docs/architecture/production/structured-failure-envelope-design.md` (canonical failure envelope)
- Plan: `docs/superpowers/plans/2026-05-20-failure-envelope-phase1.md` (failure envelope implementation)
- Spec: `docs/superpowers/specs/2026-05-21-test-reorganization-design.md` (three-tier Google Test reorganization)
- Plan: `docs/superpowers/plans/2026-05-22-test-reorganization-impl.md` (Google Test migration)
- Plan: `docs/superpowers/plans/2026-05-22-memory-region-accounting-pressure-impl.md` (memory region accounting and pressure admission)
- Architecture: `docs/architecture/production/actor-quarantine-circuit-breaker-design.md` (actor quarantine and circuit breaker)
- Plan: `docs/superpowers/plans/2026-05-23-actor-quarantine-circuit-breaker-impl.md` (actor quarantine implementation)
- Spec: `docs/superpowers/specs/2026-05-24-msg001-delivery-semantics-design.md` (delivery mode, dedup, and deadline semantics)
- Plan: `docs/superpowers/plans/2026-05-24-msg001-delivery-semantics-impl.md` (delivery semantics implementation)
- Spec: `docs/superpowers/specs/2026-05-24-coverage-cmake-option-design.md` (coverage CMake option)
- Spec: `docs/superpowers/specs/2026-05-28-fault-injection-hooks-design.md` (deterministic fault injection hooks)
- Plan: `docs/superpowers/plans/2026-05-28-fault-injection-hooks-impl.md` (fault injection implementation)
- Spec: `docs/superpowers/specs/2026-05-29-scheduler-decouple-design.md` (scheduler decoupling and hardening)
- Plan: `docs/superpowers/plans/2026-05-29-scheduler-decouple-impl.md` (scheduler decoupling implementation)
- Spec: `docs/superpowers/specs/2026-05-30-dlq-handoff-design.md` (DLQ handoff and CLI commands)
- Plan: `docs/superpowers/plans/2026-05-30-mbx-004-dlq-handoff-impl.md` (DLQ handoff implementation)
- Spec: `docs/superpowers/specs/2026-05-30-priority-mailbox-lanes-design.md` (priority mailbox lanes)
- Plan: `docs/superpowers/plans/2026-05-30-mbx-005-priority-lanes-impl.md` (priority lanes implementation)
- App Design: `docs/app/edgeops-telemetry-platform-design.md` (EdgeOps telemetry platform app design)

## Key Decisions

- Event-based actors (caf-style) with cooperative scheduling
- Explicit lifecycle with optional hibernation
- Both statically and dynamically typed actors
- Hierarchical supervision (OneForOne, AllForOne)
- Pluggable service discovery: IServiceDiscovery interface with 4 backends (gossip, registrar, static, hybrid)
- Decentralized membership via SWIM gossip protocol — no single point of failure
- Production reliability roadmap is organized into data plane, control plane, and operations plane.
- Production reliability foundation now includes delivery modes, receiver deduplication, failure envelopes, bounded mailboxes, multi-lane priority queues, DLQ with CLI replay/export, tracing correlation, deterministic fault injection (80 sites, 14 domains), graceful shutdown, lifecycle, and actor quarantine.
- Mailbox core replaced: `MultiLaneQueue<T>` lock-free multi-lane queue supersedes `MPSCMailbox` with dedicated system lane, priority-aware routing, and per-lane depth observability.
- Typed memory regions with per-region pressure admission and observability.
- Hibernation via serialization + madvise(MADV_PAGEOUT) to ZRAM for cold storage
- Actors are relocatable by ActorId, enabling slab compaction without dangling pointers
- Header-only actor-facing APIs with compiled runtime, C++20, system OpenSSL/Protobuf, and vendored llhttp/toml++/GoogleTest.
- No exceptions (-fno-exceptions), no RTTI (-fno-rtti)
- Opaque `Id<Tag, T>` aliases keep ActorId, MessageId, AlarmHandle, and timer IDs constant-initializable without cross-domain comparisons.

## Current Progress

**Core Runtime Foundation Complete** (1411 source-level GTest cases, 219 test source files in current tree)
- Phase 0: Local Message Delivery — actor spawn and local message routing
- Phase 1: ActorRef and Unified References — ActorRef as variant<Actor, ActorProxy>
- Phase 2: TCP Transport Implementation — kqueue/epoll event loop, TcpTransport, Connection
- Phase 3: Message Serialization — TypeTag enum, DefaultSerializer, Frame encode/decode
- Phase 4: Connection Pool and Handshake — TlsContext, TlsConnection, ConnectionPool, TLS handshake, AES-256-CBC encryption
- Phase 5: Service Discovery — UdpRegistrar, HostResolver, NodeRegistry, static routes, DNS resolution, RegistrarServer/RegistrarClient with TCP registration, heartbeat, failover
- Phase 6: Remote Actor Spawn — AsyncActor, ActorTypeRegistry, SpawnReceiver, spawn_remote()
- Scheduling Subsystem: ChaseLev deque, MultiPriorityWorkQueue, EDFQueue, A2WS, TimingWheel, CoroutineFramePool, HybridScheduler, WorkerThread, ActorState, CoroutineTask/CoroutinePromise, awaiters, MultiLaneQueue mailbox

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
  - reliable messaging completion (ACK/NACK, automatic retry, retry exhaustion policy, durable outbox/inbox)
  - durable actor state (snapshot, event sourcing, recovery)
- Cluster control follow-up:
  - cluster node failure model with node quarantine/fencing
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
  - `adt/` — Shared ADTs (Id, tags, NodeIdentity, MpscRingBuffer, StreamBuffer)
  - `config/` — TOML topology config (topology_model, actor_factory, actor_factory_registry, toml_parser, binary_format, binary_serializer, binary_loader, actor_args)
  - `ref/` — Actor references (address, ref, proxy)
  - `net/` — Networking (event loop, TLS, connection pool, transports)
  - `supervision/` — Supervision strategies
  - `core/` — Core runtime (actor_system, mailbox, registry)
  - `mailbox/` — MultiLaneQueue, MPSC mailbox, delivery modes, dedup cache, mailbox policy, DLQ, overflow handlers, backpressure signals
  - `sched/` — Scheduling subsystem (scheduler_interfaces, work_queue, edf_queue, a2ws, timing_wheel, coroutine_frame_pool, actor_execution_engine, actor_ready_gate, work_placement_scheduler)
  - `types/` — Type system (types, types_fwd, serialization)
  - `rpc/` — RPC channel (rpc_channel.hpp)
  - `mem/` — Memory management (alloc_header, size_class, freelist, segment_provider, slab_cache, thread_local_allocator, memory_region, memory_config, memory_tracker, telemetry_ring_buffer, hibernation_registry, hibernatable, guard_page, compaction, zram)
  - `cli/` — CLI subsystem (cli_actor, cli_config, cli_types, command_node, command_context, token, lexer, pager, output_formatter, pretty_formatter, json_formatter, tabular_formatter, commands/)
  - `metrics/` — Metrics subsystem (metrics_ring_buffer, metrics_event, metrics_config, metrics_registry, metrics_aggregator, metrics_formatter, metrics_actor)
  - `log/` — Structured logging (log_ring_buffer, logger, log_drain, log_sink, log_formatter, log_manager, log_config)
  - `tracing/` — Distributed tracing (trace_context, span, span_guard, trace_manager, trace_exporter, trace_config)
  - `fault/` — Deterministic fault injection (fault_controller, fault_schedule, fault_point, fault_types, fault_macros)
- `src/actor/` — actor_system.cpp, abstract_actor.cpp, actor_context.cpp, event_based_actor.cpp, local_actor.cpp, spawn_receiver.cpp
- `src/adt/` — shared ADT implementations
- `src/cli/` — cli_actor.cpp, lexer.cpp, command_node.cpp, pretty_formatter.cpp, json_formatter.cpp, tabular_formatter.cpp, pager.cpp
- `src/metrics/` — metrics_registry.cpp, metrics_aggregator.cpp, metrics_formatter.cpp, metrics_actor.cpp
- `src/log/` — log_manager.cpp, log_drain.cpp, log_sinks.cpp
- `src/tracing/` — trace_manager.cpp, trace_exporter.cpp
- `src/config/` — actor_factory_registry.cpp, toml_parser.cpp, binary_serializer.cpp, binary_loader.cpp
- `src/fault/` — fault_controller.cpp, fault_point_registry.cpp, fault_points.cpp, fault_schedule.cpp
- `src/mailbox/` — dedup cache, overflow handlers, backpressure signal serialization, DLQ support
- `src/net/` — event_loop.cpp, acceptor.cpp, connection.cpp, tcp_transport.cpp, frame.cpp, tls_context.cpp, tls_connection.cpp, connection_pool.cpp, registrar.cpp
- `src/ref/` — actor_proxy.cpp, actor_ref.cpp
- `src/sched/` — scheduler.cpp, worker_thread.cpp, actor_execution_engine.cpp, actor_ready_gate.cpp, work_placement_scheduler.cpp, edf_queue.cpp, a2ws.cpp, timing_wheel.cpp, coroutine_frame_pool.cpp
- `src/spawn.cpp` — AsyncActor implementation
- `src/actor_type_registry.cpp` — ActorTypeRegistry implementation
- `src/core/serialization.cpp` — DefaultSerializer implementation
- `src/rpc/rpc_channel.cpp` — RpcChannel implementation
- `src/mem/` — segment_provider.cpp, slab_cache.cpp, thread_local_allocator.cpp, memory_config.cpp, memory_tracker.cpp, hibernation_manager.cpp, guard_page.cpp, compaction.cpp, zram.cpp
- `tools/toml-compiler/` — AOT compiler executable (compiler.cpp)
- `examples/` — simple API examples
- `apps/` — complex demo applications that exercise multiple HPActor subsystems
- Tests: `tests/{unit,integration,system}/` — three-tier structure with 32 GTest binaries and 219 test source files.

## Build Commands

```bash
# Configure and build
cmake -S . -B build -GNinja
ninja -C build

# Run tests
ctest --output-on-failure --parallel 8

# Run a single test binary
./build/tests/unit/core/test_unit_core

# Run GTest with filter (tests are individual GTest cases via ctest)
./build/tests/unit/core/test_unit_core --gtest_list_tests
./build/tests/unit/core/test_unit_core --gtest_filter="*ActorId*"

# Run specific GTest case through ctest
ctest -R "ActorIdDefaultConstruction" --output-on-failure

# With sanitizers
cmake -DENABLE_TSAN=ON ..  # ThreadSanitizer
cmake -DENABLE_ASAN=ON ..  # AddressSanitizer (may show false positives in intrusive queue tests)

# Enable/disable examples (default ON)
cmake -DENABLE_EXAMPLES=OFF ..
cmake -DENABLE_APPS=OFF ..  # Disable complex demo applications (default ON)

# Memory management options
cmake -DENABLE_MEMORY_TRACKING=OFF ..  # Disable per-actor tracking (default ON)
cmake -DENABLE_MEMORY_DEBUG=ON ..     # Enable poisoning + canaries (default OFF)

# Runtime subsystem options
cmake -DENABLE_ACTOR_METRICS=OFF ..   # Disable actor-level metrics (default ON)
cmake -DENABLE_ACTOR_LOGGING=OFF ..   # Disable structured logging (default ON)
cmake -DENABLE_ACTOR_TRACING=OFF ..   # Disable distributed tracing (default ON)
cmake -DENABLE_CLI=OFF ..             # Disable interactive CLI subsystem (default ON)
cmake -DENABLE_COVERAGE=ON ..         # Enable coverage instrumentation
```

## Known Issues

- ASAN may report false positives in `test_mailbox_awaiter` and `test_priority_scheduler` due to intrusive queue memory patterns. Tests pass cleanly with TSAN or without sanitizers.
- Proactor backend (`IoUringBackend`, `GcdBackend`) needs production hardening.
