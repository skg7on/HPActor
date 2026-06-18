# Coverage Improvement — System & Integration Test Design

**Date:** 2026-06-18
**Status:** Design approved
**Issue:** [#325](https://github.com/skg7on/HPActor/issues/325)

## Current Coverage Baseline (2026-06-17)

| Metric | Coverage | Hit / Total | Rating |
|--------|----------|-------------|--------|
| Lines | 65.7% | 15,141 / 23,033 | <75% — Low |
| Functions | 79.3% | 2,882 / 3,633 | 75–90% — Medium |
| Branches | 49.7% | 5,255 / 10,564 | <75% — Low |

Target: 80% lines, 85% functions, 65% branches.

## Design Approach

**Strategy:** Workflow-based system and integration tests that exercise multiple
subsystems together. Each test covers a complete end-to-end workflow, maximizing
coverage-per-test by traversing the full stack rather than isolating single
components.

**Five workflows** targeting the lowest-coverage areas:

| # | Workflow | Tier | Test file | Tests |
|---|----------|------|-----------|-------|
| A | HTTP API → CRUD → DLQ → Shutdown | System | `test_system_http_api_workflow.cpp` | 22 |
| B | Supervision → Failure → Quarantine | System | `test_system_supervision_workflow.cpp` | 9 |
| C | RPC → Transport → Timeout → Retry | System | `test_system_rpc_workflow.cpp` | 10 |
| D | Process → Daemon → Health → Watchdog | Integration | `test_process_lifecycle.cpp` | 10 |
| E | Durable State → Snapshot → Restore | Integration | `test_durable_state_workflow.cpp` | 11 |

**Total: 62 tests across 5 files.**

---

## Architecture Note: HTTP Handler OO Refactor

As of commit `9c0b3418` on `fix/client-commands-dispatch`, the HTTP handlers
were refactored from C-style free functions to polymorphic `IHttpHandler`
classes with a `HttpHandlerRegistry` singleton. Key design elements:

- **`IHttpHandler`** — pure virtual `handle(CliHttpServerActor&, HTTPConnection&, HttpRequest&&)`
- **`HttpHandlerRegistry`** — singleton owning `unique_ptr<IHttpHandler>` instances,
  exposing `routes()` as `vector<Entry{method, pattern, handler*}>`
- **Handler classes** — e.g., `GetFaultsHandler final : IHttpHandler` with
  `static constexpr kMethod`/`kPath` members
- **Lazy registration** — `register_*_handlers()` functions called from
  `init_routes()`, avoiding static initialization order issues
- **Dispatch** — `dispatch_route()` iterates registry routes, calls
  `entry.handler->handle(actor, conn, req)`

This makes handlers directly testable: instantiate the handler class, create
dependencies, call `handle()`, assert on the captured HTTP response.

### Test Double: TestHttpConnection

A concrete `net::HTTPConnection` subclass that captures the response in memory,
avoiding real socket I/O:

```cpp
class TestHttpConnection : public net::HTTPConnection {
    net::HttpResponse last_response_;
    bool response_sent_ = false;
public:
    void send_response(net::HttpResponse&& resp) override {
        last_response_ = std::move(resp);
        response_sent_ = true;
    }
    net::HttpResponse& last_response() { return last_response_; }
    bool response_sent() const { return response_sent_; }
};
```

---

## Workflow A: HTTP API → CRUD → DLQ → Shutdown

**File:** `tests/system/test_system_http_api_workflow.cpp`

**Zero-coverage code targeted:**
- `src/cli/handlers/cli_http_actor_handlers.cpp` (0%)
- `src/cli/handlers/cli_http_system_handlers.cpp` (0%)
- `src/cli/handlers/cli_http_dlq_handlers.cpp` (0%)
- `src/cli/handlers/cli_http_fault_handlers.cpp` (0%)
- `src/cli/handlers/cli_http_ask_handlers.cpp` (0%)
- `src/cli/handlers/cli_http_legacy_handler.cpp` (0%)
- `src/cli/handlers/cli_http_handler_helpers.cpp` (0%)
- `src/cli/handlers/cli_http_handler_helpers.hpp` (0%)
- `src/cli/http_handler.cpp` (0% — new file from refactor)
- `include/hpactor/cli/http_handler.hpp` (0% — new file from refactor)
- `src/cli/cli_http_server_actor.cpp` (24.8%)
- `src/metrics/metrics_actor.cpp` (6.8%)

### Test scenarios

**A.1 — `HandlerRegistryPopulatedAfterRegistration`**
Call all 6 `register_*_handlers()` functions. Verify
`HttpHandlerRegistry::instance().routes()` has expected number of entries
(~25). Verify each entry: non-null handler pointer, valid HTTP method,
non-empty path pattern. Tests `HttpHandlerRegistry`, all `register_*` functions.

**A.2 — `ApiIndexHandlerReturnsEndpointList`**
Instantiate `ApiIndexHandler`. Construct `GET /api/v1` request.
Call `handler.handle(actor, conn, req)`. Verify 200. Verify JSON body
contains entries for "actors", "system", "dlq", "faults", "asks"
endpoint groups. Tests `ApiIndexHandler`.

**A.3 — `GetSystemHandlerReturnsSystemInfo`**
Instantiate `GetSystemHandler`. Spawn several actors. Call `handle()`
with `GET /api/v1/system`. Verify 200. Verify JSON contains
total_actors count, worker_count, uptime fields. Tests `GetSystemHandler`.

**A.4 — `GetSystemStatsHandler`**
Instantiate `GetSystemStatsHandler`. Call `handle()`. Verify 200.
Verify JSON contains message count and mailbox stats fields.
Tests `GetSystemStatsHandler`.

**A.5 — `SystemMemoryHandler`**
Instantiate `GetSystemMemoryHandler`. Call `handle()`. Verify 200.
Verify JSON contains memory region data keyed by region name.
Tests `GetSystemMemoryHandler`.

**A.6 — `DrainAndShutdownHandlers`**
Instantiate `DrainHandler`. Call `handle()` with POST. Verify 202 accepted.
Instantiate `ShutdownHandler`. Call `handle()` with POST. Verify 202.
Tests `DrainHandler`, `ShutdownHandler`.

**A.7 — `ListActorsHandlerWithPagination`**
Register and spawn test actors. Instantiate `ListActorsHandler`.
Call with `GET /api/v1/actors`. Verify 200 and paginated actor list.
Verify actor metadata fields (id, type, state). Call with
`?page=2&page_size=5`. Verify pagination params parsed correctly.
Tests `ListActorsHandler`, pagination helpers.

**A.8 — `GetActorHandlerWithInspect`**
Spawn an actor. Instantiate `GetActorHandler`. Call with
`GET /api/v1/actors/:id` using valid path_params. Verify 200.
Verify JSON contains metadata, mailbox, and lifecycle inspect fields.
Tests `GetActorHandler`.

**A.9 — `KillActorHandler`**
Spawn an actor. Instantiate `KillActorHandler`. Call with
`DELETE /api/v1/actors/:id`. Verify 200. Verify actor is terminated.
Tests `KillActorHandler`.

**A.10 — `GetMailboxAndChildrenHandlers`**
Spawn a supervisor with children. Instantiate `GetMailboxHandler`.
Call with valid actor ID. Verify 200 and mailbox snapshot fields
(depth, capacity, pressure state). Instantiate `GetChildrenHandler`.
Call with same actor ID. Verify 200 and child list.
Tests `GetMailboxHandler`, `GetChildrenHandler`.

**A.11 — `CircuitBreakerHandlers`**
Configure actor with circuit breaker policy. Instantiate
`GetCircuitBreakerHandler`. Call `GET`. Verify circuit breaker state
in response. Instantiate `ResetCircuitBreakerHandler`. Call `POST`.
Verify 200 and circuit breaker reset.
Tests `GetCircuitBreakerHandler`, `ResetCircuitBreakerHandler`.

**A.12 — `QuarantineHandlers`**
Instantiate `QuarantineHandler`. Call `POST /api/v1/actors/:id/quarantine`.
Verify 200 and actor quarantined. Instantiate `UnquarantineHandler`.
Call `DELETE /api/v1/actors/:id/quarantine`. Verify 200 and actor released.
Tests `QuarantineHandler`, `UnquarantineHandler`.

**A.13 — `GetActorMemoryHandler`**
Spawn an actor. Instantiate `GetActorMemoryHandler`. Call `GET`.
Verify 200 and memory stats in response (allocated bytes, block count).
Tests `GetActorMemoryHandler`.

**A.14 — `DlqHandlersFullWorkflow`**
Configure bounded mailbox with DLQ overflow policy. Send messages
until overflow occurs. Instantiate `ListDlqHandler`. Call `GET /api/v1/dlq`.
Verify 200 and records present. Instantiate `GetDlqRecordHandler`.
Call `GET /api/v1/dlq/:index`. Verify 200 and record detail.
Instantiate `ReplayDlqHandler`. Call `POST /api/v1/dlq/:index/replay`.
Verify 200 and message replayed. Instantiate `ExportDlqHandler`.
Call `GET /api/v1/dlq/export`. Verify 200 and export format.
Tests all 4 DLQ handler classes.

**A.15 — `FaultHandlers`**
Enable fault injection. Instantiate `GetFaultsHandler`. Call `GET`.
Verify 200 and enabled=true, hooks_triggered in response. Inject a fault.
Verify hooks_triggered incremented on next GET. Instantiate
`ClearFaultsHandler`. Call `POST /api/v1/faults/clear`. Verify 200.
Verify hooks_triggered reset on next GET.
Tests `GetFaultsHandler`, `ClearFaultsHandler`.

**A.16 — `AskHandlersReturnNotImplemented`**
Instantiate `ListAsksHandler`. Call `GET /api/v1/asks`. Verify response
indicates "not implemented" (503 or 501). Same pattern for
`GetAskHandler` (`GET /api/v1/asks/:message_id`) and `CancelAskHandler`
(`DELETE /api/v1/asks/:message_id`). Tests the 3 ask stub handler classes.

**A.17 — `LegacyCliHandler`**
Instantiate `LegacyPostCliHandler`. Construct `POST /cli` with JSON body
containing a CLI command. Call `handle()`. Verify 200. Verify the
command was executed and response contains expected output.
Tests `LegacyPostCliHandler`.

**A.18 — `FullDispatchViaRouteTable`**
Call all `register_*_handlers()`. Create CliHttpServerActor and call
`init_routes()`. Build 5 HttpRequest objects for different endpoints
(actors, system, dlq, faults, asks). Call `dispatch_route(conn, req)`
for each. Verify correct handler invoked (check response).
Verify 404 for unknown route. Tests end-to-end: registration →
pattern matching → handler dispatch.

**A.19 — `RoutePatternMatching`**
Test `match_route_pattern()` directly:
- Exact match: `/api/v1/actors` matches `/api/v1/actors`
- `:id` param extraction: `/api/v1/actors/:id` matches `/api/v1/actors/42`
  with params `{id: "42"}`
- Multi-segment: `/api/v1/actors/:id/mailbox` matches correctly
- No match: `/api/v1/actors` does not match `/api/v1/actors/extra/path`
- Missing segment: `/api/v1/actors/:id` does not match `/api/v1/actors`
Tests `match_route_pattern()` from `http_handler.hpp`.

**A.20 — `ErrorResponseHelpers`**
Call `send_error()` with 400, 404, 500. Verify response format
(status code, JSON body with error code and message).
Call `send_json_ok()` — verify 200, content-type=application/json.
Call `send_accepted()` — verify 202.
Call `send_success()` — verify 200 with success message.
Tests `cli_http_handler_helpers.cpp` response helpers.

**A.21 — `ContentTypeValidation`**
Call `validate_json_content_type()` with `application/json` — verify true.
Call with `text/plain` — verify false, verify error response sent.
Call with missing Content-Type header — verify false, verify 400 response.
Tests `validate_json_content_type()` helper.

**A.22 — `ActorIdParsingAndValidation`**
Call handler with valid actor ID in path_params — verify actor found.
Call with non-existent actor ID — verify 404 response.
Call with malformed/empty ID — verify 400 response.
Tests `parse_actor_id_param()` and related validation helpers.

---

## Workflow B: Supervision Tree → Failure → Restart/Quarantine

**File:** `tests/system/test_system_supervision_workflow.cpp`

**Low-coverage code targeted:**
- `src/supervision/supervision.cpp` (43.6%)
- `src/actor/lifecycle/*` (68.6%)
- `src/config/parsers/quarantine_parser.cpp` (16.0%)
- `src/config/parsers/passivation_config_parser.cpp` (19.0%)

### Test scenarios

**B.1 — `SupervisorRestartsFailedChild`**
Create OneForOne supervisor with 3 children. Send message that causes
child[1] to fail. Verify child[1] transitions through
Failed→Starting→Running. Verify child[0] and child[2] are unaffected.
Verify supervisor DownMsg handling. Verify restart counted.
Exercises: `SupervisorActor::handle_child_down()` Restart path,
`restart_child()`, lifecycle transitions.

**B.2 — `AllForOneSupervisorRestartsAll`**
Create AllForOne supervisor with 3 children. One child fails.
Verify ALL children restart. Verify restart order (topological).
Verify sibling state transitions.
Exercises: `AllForOneSupervisor::on_child_failure()`, `restart_all_children()`.

**B.3 — `SupervisorEscalatesAfterMaxRestarts`**
Configure max_restarts=3 within 5s window. Child fails 4 times rapidly.
Verify first 3 failures trigger restart. Verify 4th failure triggers
escalate. Verify supervisor stops the child. Verify failure counting
and window tracking.
Exercises: restart limit/escalation branch in `decide_restart()`.

**B.4 — `SupervisorQuarantinesFailingChild`**
Configure quarantine policy on child. Child fails repeatedly.
Verify after restart limit exhaustion, child is quarantined (not restarted).
Verify `FailureReason::Quarantined`. Verify quarantine state in child
lifecycle.
Exercises: quarantine escalation path in `handle_child_down()`.

**B.5 — `SelfSupervisingActorManagesOwnChildren`**
SelfSupervisingActor with 2 children. One child fails. Verify
self-supervising actor handles DownMsg. Verify child restarted.
Verify parent tracks children correctly. Verify remote child tracking.
Exercises: `SelfSupervisingActor::handle_child_down()`, `decide_restart()`,
`add_child/remove_child`, `add_remote_child/remove_remote_child`.

**B.6 — `CircuitBreakerTripsAndResets`**
Child with circuit breaker. Trigger consecutive failures. Verify
circuit transitions: Closed → Open. Verify request rejected while Open.
Wait for cooldown. Verify HalfOpen. Send probe message. Verify success
transitions back to Closed. Verify failure transitions back to Open.
Exercises: `CircuitBreakerTracker`, `FailureRateTracker`, EMA computation.

**B.7 — `SupervisionTreeDeepNesting`**
Create 3-level supervision tree (root → mid → leaf). Leaf fails.
Verify mid-level supervisor restarts leaf. Mid-level supervisor fails.
Verify root restarts mid (and mid restarts leaf). Verify proper death
propagation through levels.
Exercises: hierarchical restart cascading.

**B.8 — `SupervisionWithScheduledMessages`**
Child with pending scheduled message. Child fails. Supervisor restarts
child. Verify scheduled message still delivers after restart.
Verify AlarmHandle survival across restart.
Exercises: supervision + timer subsystem interaction.

**B.9 — `SupervisorHandlesStopDirective`**
Child returns Stop directive on failure. Supervisor stops child
(no restart). Verify DownMsg with Stop. Verify child lifecycle
ends at Terminated.
Exercises: Stop directive path in `handle_child_down()`.

---

## Workflow C: Remote RPC → Transport → Timeout → Retry

**File:** `tests/system/test_system_rpc_workflow.cpp`

**Low-coverage code targeted:**
- `include/hpactor/rpc/rpc_channel.hpp` (0%)
- `include/hpactor/rpc/rpc_types.hpp` (0%)
- `src/rpc/rpc_channel.cpp` (71.4% — remaining uncovered branches)
- `src/net/reactor/epoll_backend.cpp` (52.0%)
- `src/net/` (64.2%)
- `include/hpactor/net/` (78.2% lines / 39.4% branches)
- `src/config/parsers/delivery_config_parser.cpp` (14.8%)

### Test scenarios

**C.1 — `RpcLocalRequestResponse`**
Send RPC request via `ActorContext::rpc()`. Local actor receives request.
Local actor replies. `RpcFuture` resolves. Verify response payload.
Verify no network path taken.
Exercises: `RpcChannel::call_raw()` local path, `on_response()`, `RpcFuture::get()`.

**C.2 — `RpcRemoteRequestResponse`**
Start two ActorSystems on loopback. Send RPC from system A to actor
on system B. Actor on B replies. Verify response on A. Verify frame
encoding/decoding. Verify connection established.
Exercises: remote transport path, frame serialization, `RpcResponseFrame` routing.

**C.3 — `RpcTimeoutWithRetry`**
Configure RPC with 100ms timeout, max_retries=2. Send RPC to actor that
never replies. Verify first timeout fires. Verify retry scheduled.
Verify retry count increments. Verify second timeout. Verify max retries
exhausted. Verify `RpcFuture::get()` returns error. Verify
`DeadLetterReason::AskTimeout`.
Exercises: `on_timeout()`, `schedule_retry()`, retry exhaustion, `PendingCall` transitions.

**C.4 — `RpcTimeoutZeroRetries`**
Configure RPC with 100ms timeout, max_retries=0. Send RPC to non-replying
actor. Verify timeout. Verify no retry attempted. Verify immediate error.
Exercises: no-retry path.

**C.5 — `RpcAbortBeforeResponse`**
Send RPC. Call `abort()` before response arrives. Verify `RpcFuture`
resolves with cancelled error. Verify no further retries. Verify
`PendingCall` cleaned up.
Exercises: `RpcChannel::abort()` path.

**C.6 — `RpcConcurrentCalls`**
Send 5 concurrent RPCs to same actor. All reply in different order.
Verify all futures resolve correctly. Verify response-to-request
correlation by message_id. Verify no cross-talk.
Exercises: `PendingCall` map management, concurrent correlation.

**C.7 — `RpcIdempotentRetry`**
Configure idempotent RPC. Send RPC. Timeout. Retry. Response arrives
from retry. Verify success.
Exercises: idempotent flag path, retry with eventual success.

**C.8 — `RpcResponseBeforeTimeoutRace`**
Send RPC with short timeout. Response arrives just before timeout fires.
Verify response delivered, not timeout. Verify no spurious retry.
Exercises: race condition between response arrival and timeout timer.

**C.9 — `RpcDeliveryModeConfiguration`**
Parse TOML config with delivery settings. Verify `DeliveryMode` parsed
correctly. Send RPC with at_least_once delivery. Verify retry behavior
matches delivery mode.
Exercises: `delivery_config_parser.cpp`.

**C.10 — `RpcConnectionFailure`**
Start two ActorSystems. Send RPC. Shutdown system B mid-flight.
Verify connection failure. Verify appropriate error on A.
Verify no infinite retry loop.
Exercises: transport error paths, connection failure handling.

### Test helper

`DelayedResponseActor` — an EventBasedActor that can be configured to
delay its reply by N milliseconds (using schedule + self-message), or
to never reply.

---

## Workflow D: Process Lifecycle → Daemon → Health HTTP → Watchdog

**File:** `tests/integration/process/test_process_lifecycle.cpp`

**Low-coverage code targeted:**
- `src/process/process_manager.cpp` (55.6% — remaining uncovered branches)
- `src/process/health_http_server.cpp` (0%)
- `src/process/watchdog_actor.cpp` (0%)
- `include/hpactor/process/health_http_server.hpp` (0%)
- `include/hpactor/process/watchdog_actor.hpp` (0%)
- `src/config/parsers/process_config_parser.cpp` (22.2%)

### Design constraints

- No actual daemonization fork — test foreground mode and non-fork paths
- Signal handling tested via direct function calls where possible
- Pidfile tested using temp directories
- Platform-specific code guarded with `#if defined(__linux__)` / `#elif defined(__APPLE__)`

### Test scenarios

**D.1 — `ProcessManagerForegroundInit`**
Create `ProcessConfig` with `ProcessMode::Foreground`. Call
`ProcessManager::init(config)`. Verify no fork occurs. Verify pidfile
written. Verify signal handlers installed. Verify working directory
unchanged.

**D.2 — `ProcessManagerPidfileLifecycle`**
Init with pidfile path in temp dir. Verify pidfile created with correct
PID. Verify atomic write (temp file + rename, no .tmp residue).
Call cleanup. Verify pidfile removed.

**D.3 — `ProcessManagerNotifyReady`**
Init in foreground mode. Call `ProcessManager::notify_ready()`. Verify
sd_notify path executes without error (no `NOTIFY_SOCKET` set).
Call `notify_status("custom status")`. Verify no crash.
Exercises: `notify_ready()`, `notify_status()`, `notify_stopping()`.

**D.4 — `ProcessManagerSignalHandling`**
Init with signal handlers. Verify SIGTERM/SIGINT handler installed.
Verify SIGHUP handler installed. Call handler functions directly.
Verify they set expected flags/state.

**D.5 — `ProcessManagerWaitForSignal`**
Init. Post a signal to signalfd (Linux) or sigwait queue (macOS).
Call `wait_for_signal()`. Verify correct signal returned.
Exercises both platform code paths.

**D.6 — `ProcessConfigParsing`**
Parse TOML with `[system.process]` section: mode="daemon",
pidfile="/run/hpactor.pid", watchdog_interval_ms=5000. Verify
`ProcessConfig` fields. Verify `ProcessMode` enum. Verify defaults
for unspecified fields.
Exercises: `process_config_parser.cpp`.

**D.7 — `HealthHttpServerStartAndRespond`**
Create ActorSystem with health HTTP on loopback:0. Spawn
HealthHttpServer. Send HTTP GET /health. Verify 200 OK.
Verify response body contains health status. Verify content-type.
Exercises: `health_http_server.cpp`, `health_http_server.hpp`.

**D.8 — `HealthHttpServerAllPathsReturnOk`**
Start health server. Send GET /. Verify 200. Send GET /ready.
Verify 200. Send GET /healthz. Verify 200. Send GET /anything.
Verify 200 (responds OK to all paths).

**D.9 — `WatchdogActorPeriodicNotify`**
Create ActorSystem with watchdog enabled (watchdog_interval=100ms).
Spawn WatchdogActor. Verify initialization. Verify
`ProcessManager::notify_watchdog()` is called at least once within 500ms.
Verify actor continues running normally.
Exercises: `watchdog_actor.cpp`.

**D.10 — `ProcessConfigAllModes`**
Parse configs for Foreground, Systemd, and Daemon modes. Verify each
enum value. Verify mode-specific fields (watchdog enabled for Systemd,
pidfile required for Daemon).

---

## Workflow E: Durable State → Snapshot → Restore → File Store

**File:** `tests/integration/actor/test_durable_state_workflow.cpp`

**Low-coverage code targeted:**
- `src/actor/durable/file_state_store.cpp` (0%)
- `src/actor/durable/in_memory_state_store.cpp` (91.8% — remaining branches)
- `include/hpactor/actor/durable/file_state_store.hpp` (0%)
- `include/hpactor/actor/durable/durable_actor.hpp` (20.0%)

### Test scenarios

**E.1 — `InMemoryStoreSnapshotWriteAndLoad`**
Create `InMemoryStateStore`. Write snapshot with test data.
Verify write succeeds. Load snapshot. Verify data matches.
Exercises: remaining branches in `in_memory_state_store.cpp`.

**E.2 — `InMemoryStoreEventAppendAndLoad`**
Write snapshot. Append 5 events. Load events from sequence 2.
Verify events 2-4 returned. Append more events. Verify total count.
Exercises: event persistence paths.

**E.3 — `InMemoryStoreDelete`**
Write snapshot and events. Delete persistence ID. Verify snapshot gone.
Verify events gone. Verify delete idempotent (no crash).
Exercises: delete path.

**E.4 — `FileStoreSnapshotWriteAndLoad`**
Create `FileStateStore` with temp directory. Write snapshot with CRC32C
checksum. Verify file created on disk. Verify atomic write pattern
(temp file + rename, no .tmp files left). Load snapshot. Verify data
and checksum match.
Exercises: main path of `file_state_store.cpp`.

**E.5 — `FileStoreEventAppendAndLoadAfter`**
Write snapshot. Append events. Verify events persisted to disk.
Load events from sequence. Verify correct range returned.
Exercises: event persistence paths in file store.

**E.6 — `FileStoreCorruptedChecksum`**
Write snapshot. Manually corrupt the file on disk. Attempt to load.
Verify checksum error detected. Verify appropriate error returned
(no crash/UB).
Exercises: CRC32C validation and error handling.

**E.7 — `FileStoreMissingFile`**
Attempt to load snapshot for non-existent persistence ID. Verify
"not found" error (not crash). Verify empty events for non-existent ID.
Exercises: missing file error path.

**E.8 — `FileStoreDeleteCleansUp`**
Write snapshot + events. Verify files exist on disk. Delete persistence ID.
Verify files removed from disk. Verify directory cleaned if empty.
Exercises: delete/cleanup path.

**E.9 — `FileStoreConcurrentWriteSafety`**
Create two `FileStateStore` instances for same directory. Write from
store A. Read from store B. Verify store B sees store A's data.
Verify atomic rename prevents partial reads.
Exercises: atomic-rename consistency guarantee.

**E.10 — `DurableActorInterfaceContract`**
Implement minimal `IDurableActor`. Verify `persistence_id()`.
Verify `snapshot_state()` serialization. Verify `restore_snapshot()`
deserialization. Verify `apply_event()`. Verify `migrate_snapshot()`
compatibility check.
Exercises: `durable_actor.hpp` (currently 20%).

**E.11 — `FileStoreLargeSnapshot`**
Write large snapshot (1MB+). Verify CRC32C computed correctly.
Load. Verify data integrity. Verify no buffer overflow.
Exercises: large-data paths.

---

## Test Infrastructure

### Existing helpers used

| Helper | Source | Used by |
|--------|--------|---------|
| `test::minimal_config()` | `system_test_fixture.hpp` | All system tests |
| `test::config_with_scheduler(n)` | `system_test_fixture.hpp` | Workflows B, C |
| `test::assert_eventually()` | `system_test_fixture.hpp` | Workflows C, D |
| `SchedulerTestDriver` | `scheduler_test_driver.hpp` | Workflows B, C |
| `test::CountingActor` | `system_test_fixture.hpp` | Workflows A, B |
| `test::EchoActor` | `system_test_fixture.hpp` | Workflows A, B |
| `test::FailingActor` | `system_test_fixture.hpp` | Workflow B |
| `test::ForwardingActor` | `system_test_fixture.hpp` | Workflows A, C |

### New test infrastructure needed

| Component | File | Used by |
|-----------|------|---------|
| `TestHttpConnection` | New, in `test_system_http_api_workflow.cpp` or shared header | Workflow A |
| `DelayedResponseActor` | New, in `test_system_rpc_workflow.cpp` | Workflow C |

### Build integration

- **System tests** (A, B, C): Add source files to
  `tests/system/CMakeLists.txt` — they compile into the existing
  `test_system` executable
- **Integration tests** (D, E): Add source files to
  `tests/integration/process/CMakeLists.txt` and
  `tests/integration/actor/CMakeLists.txt` respectively — each directory
  has its own GTest executable

---

## Implementation Order

1. **Workflow D** (Process) — simplest, fewest dependencies, establishes the pattern
2. **Workflow E** (Durable State) — focused, temp-dir-based, no scheduler needed
3. **Workflow B** (Supervision) — core actor framework, uses SchedulerTestDriver
4. **Workflow C** (RPC) — requires transport, more complex setup
5. **Workflow A** (HTTP API) — largest, requires TestHttpConnection, 22 tests

Each workflow is independently testable and mergeable.
