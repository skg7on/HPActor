# Phase 6 Baseline: Constructor/Topology/Shutdown/Destructor Side-Effect Ledger

**Date:** 2026-06-30
**Branch:** `refactor/actor-system-runtime-lifecycle`
**Baseline commit:** `6cb098a85` (Phase 5 merge)

## 1. Constructor Side Effects (in order)

`ActorSystem::ActorSystem(const Config&)` — ~290 lines in `src/actor/actor_system.cpp:125-415`

| Step | Side Effect | Reversible? | Notes |
|------|-----------|-------------|-------|
| 1 | `core.config = config` | Yes (value) | Mutable copy |
| 2 | `core.endpoint = config.endpoint` | Yes (value) | |
| 3 | `core.start_time = now()` | Yes (value) | |
| 4 | Create `HybridScheduler` | Yes (stop + join) | Thread creation deferred to `start()` |
| 5 | Create `ActorTypeRegistry` | Yes (destroy) | |
| 6 | `spawner.emplace()` with null metrics/logger | Yes (reset) | Deferred construction |
| 7 | `proto_registry.register_system_types()` | No (global-ish) | Modifies static proto registry |
| 8 | Create metrics `MpscRingBuffer` | Yes (destroy) | Conditional on metrics_config.enabled |
| 9 | `scheduler->set_metrics_ring_buffer()` | Yes (clear) | |
| 10 | Bind `messaging_ports` (reliable_ack, backpressure) | Yes (clear) | Context = &legacy network state |
| 11 | Create `MessagingRuntime` (7 components) | Yes (destroy) | DLQ, Dedup, Trackers, Backpressure, Pipeline, Engine |
| 12 | Spawn `MetricsActor` | Yes (kill) | Conditional |
| 13 | Create `LogManager` + `start()` | **Irreversible** (thread starts) | Conditional |
| 14 | `scheduler->set_logger()` | Yes (clear) | |
| 15 | Reconstruct `spawner` with metrics/logger | Yes (reset) | |
| 16 | `fault_controller.set_log_manager()` | Yes (clear) | |
| 17 | `ProcessManager::init()` | **Irreversible** (may fork) | Daemonization before threads |
| 18 | `scheduler->start()` | **Irreversible** (worker threads) | |
| 19 | `apply_tracing_config()` → create TraceManager + start() | **Irreversible** (thread) | Conditional |
| 20 | Create `NetworkRuntime` | Yes (stop + destroy) | Conditional on enable_network |
| 21 | `transport->set_metrics_ring_buffer()` | Yes (clear) | |
| 22 | `network_->start()` | **Irreversible** (network thread, listen) | Conditional |
| 23 | Create `AskManager` | Yes (destroy) | Conditional on network |
| 24 | Spawn `HTTPGatewayActor` | Yes (kill) | Conditional |
| 25 | Construct + adopt `SpawnReceiver` | Yes (kill) | Manual setup, TODO for Phase 6 |
| 26 | Create `PassivationManager` | Yes (destroy) | |
| 27 | Spawn `CliActor` | Yes (kill) | Conditional on foreground mode |
| 28 | Spawn `Receptionist` | Yes (kill) | Conditional |
| 29 | `fault_controller.install()` | Yes (remove) | |
| 30 | Create `ShutdownCoordinator` | Yes (destroy) | Captures lambdas into ActorSystem |

**Key findings:**
- Steps 7-29 happen between scheduler thread start and shutdown coordinator creation
- No coordinator owns the startup sequence — ordering is implicit in constructor code
- Network publication (step 22) happens before system actors (steps 24-25) and before readiness
- Metrics ring buffer created before MessagingRuntime, but LogManager starts after
- Spawner reconstructed in step 15 because metrics/logger were null at step 6

## 2. Destructor Side Effects (in order)

`ActorSystem::~ActorSystem()` — `src/actor/actor_system.cpp:417-445`

| Step | Action |
|------|--------|
| 1 | `core.running = false` |
| 2 | `network_->stop(Abort)` (Phase 5 path) |
| 3 | Legacy: `event_loop->stop()` |
| 4 | Legacy: `network_thread.join()` |
| 5 | Legacy: `transport->stop_listening()` |
| 6 | Legacy: `discovery->stop()` |
| 7 | `log_manager->stop()` |
| 8 | `trace_manager->stop()` |
| 9 | `scheduler->stop()` |
| 10 | `fault_controller.remove()` |

**Key findings:**
- Destructor has its own stop sequence, independent of ShutdownCoordinator
- Phase 5 NetworkRuntime covered, but legacy NetworkRuntimeState has separate cleanup
- No drain phase — calls `Abort`
- Member destruction order handles remaining cleanup (reverse declaration order in Impl)

## 3. Shutdown Side Effects

`ActorSystem::shutdown()` — delegates to `ShutdownCoordinator::execute()`:
`src/actor/lifecycle/shutdown_coordinator.cpp:108-207`

| Phase | Actions |
|-------|---------|
| DrainingIngress | `set_ready(false)`, user phases |
| DrainingActors | Pass 1: non-system actors drain, Pass 2: system actors drain |
| LeavingCluster | `leave_discovery()`, `stop_remote_runtime()` (lightweight: stop event loop) |
| FlushingTelemetry | `flush_telemetry()` (currently no-op) |
| Stopped | `running = false` |
| ForcedStop | On timeout: immediate `running = false` |

**Key findings:**
- Shutdown coordinator uses `std::function` callbacks — not fixed-size ports
- `stop_remote_runtime` only stops event loop, not full NetworkRuntime teardown
- Worker threads are not joined — scheduler keeps running during drain
- Destructor handles the heavy teardown AFTER shutdown coordinator finishes
- No rollback on partial startup failure

## 4. load_topology Side Effects (in order)

`ActorSystem::load_topology()` — `src/actor/actor_system.cpp:1191-1274`

| Step | Side Effect | Issue |
|------|-----------|-------|
| 1 | `TomlParser::parse()` | Side-effect-free (parse only) |
| 2 | Mutate `metrics_config` | After scheduler/telemetry started |
| 3 | Mutate `logging_config` | After LogManager started |
| 4 | Mutate system fields via X-macro | After scheduler started |
| 5 | Mutate mailbox fields via X-macro | After mailboxes created |
| 6 | `messaging_->reconfigure(dead_letters)` | Stable identity preserved (Phase 3) |
| 7 | `apply_tracing_config()` | May stop/restart TraceManager thread |
| 8 | Mutate `config_.process` | After daemonization |
| 9 | Mutate `config_.pool` + `transport->set_pool_config()` | After transport started |
| 10 | Validate actor factories | After mutations 2-9 applied |
| 11 | `spawn_configured()` for each actor | Partially applied if late actor fails |
| 12 | `registry.put()` for each actor | Name registration |
| 13 | Deliver `SystemInit` to each spawned actor | |

**Key findings:**
- Steps 2-9 mutate runtime state BEFORE actor factory validation (step 10)
- Late actor factory failure leaves mutations 2-9 applied with no rollback
- config, metrics, logging, mailbox, tracing, process, pool all mutated
- No diff/classification before application
- No prepare/commit/rollback transaction

## 5. Readiness & Lifecycle State

| Atomic | Initial Value | Writers |
|--------|--------------|---------|
| `core.running` | `true` | Constructor completion, ShutdownCoordinator, Destructor |
| `core.shutdown_phase` | `Running` | ShutdownCoordinator |
| `core.is_ready` | `true` | ShutdownCoordinator only during drain |

**Issue:** `is_ready` defaults to `true` — meaning the system appears "ready" even before the constructor completes. Network publication can expose the node before SystemInit is delivered.

## 6. Process Preflight Constraint

`ProcessManager::init()` at step 17 of constructor:
- Must occur BEFORE `scheduler->start()` (step 18)
- May fork (daemonize), which invalidates threads
- Currently interleaved with telemetry initialization (steps 12-16)

## 7. Characterization Tests Needed

- [ ] Constructor order verification (side-effect sequence)
- [ ] Destructor vs ShutdownCoordinator divergence
- [ ] load_topology partial mutation on actor factory failure
- [ ] is_ready default-true during construction
- [ ] Legacy constructor produces valid stopped object when network fails
- [ ] ProcessManager::init called before scheduler->start
