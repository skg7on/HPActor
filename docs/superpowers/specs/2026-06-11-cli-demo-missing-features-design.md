# CLI Demo Missing Features — Design

## 1. Overview

The CLI demo app (`apps/cli_demo/`) showcases HPActor's production features
through 10 actors across 8 types and ~30 CLI commands. An audit of the demo
against the framework's public inspection APIs reveals two categories of gaps:

1. **CLI command gaps** — commands that are registered but produce stubs
   (`/metrics show`, `/topology show`, `/ask pending/cancel/stats`,
   `/system memory`, `/system endpoints`) or commands that should exist given
   available framework APIs (`/tracing status`, `/memory regions`,
   `/scheduler workers`, `/actor <id> backpressure`, `/actor <id> links`).

2. **Demo app feature gaps** — production features that the framework fully
   supports but the demo actors don't exercise (ask/request-response, tracing,
   structured logging, coroutine actors, lifecycle state machine, typed actors,
   endpoint circuit breakers).

This design specifies the implementation for both categories, organized as
independent phases that can be implemented incrementally.

### 1.1 Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| CLI command scope | Implement only when framework inspection API exists | Don't create fake data; every command must query real runtime state |
| `/system memory` | Query `MemoryRegionRegistry` singleton directly | The singleton has `snapshot(region)` per region — no ActorSystem accessor needed |
| `/metrics show` | Drain `MetricsActor` ring buffer on-demand | Follow the existing `/metrics` scrape pattern: drain → snapshot → format |
| `/topology show` | Add `const TopologyModel*` accessor to `ActorSystem` | The model is discarded after bootstrap today; must be retained |
| `/ask pending` | Add `snapshot()` to `AskManager` | Current API only exposes `pending_count()`; need a lightweight snapshot like `OutboundDeliveryTracker` |
| `/ask cancel` | Add `cancel(MessageId)` to `AskManager` | Mirror `OutboundDeliveryTracker::cancel()` |
| `/ask stats` | Add stats counters to `AskManager` | Track registered/resolved/timed out/cancelled totals |
| `/actor <id> links` | Extend `InspectStateRequest` with `include_links` | Follow the existing inspect pattern; actors track links internally |
| `/actor <id> backpressure` | Extend `InspectStateRequest` with `include_backpressure` | Mailbox already tracks pressure state in `MboxSnapshot` |
| `/tracing status` | Query `TraceManager` via existing `ActorSystem::trace_manager()` | Read-only config + dropped-span counter already public |
| Demo app asks | Add a `QueryActor` that sends `ask()` to `ClockActor` | ClockActor already has request-response (TimeQuery→TimeReply); minimal change |
| Demo app tracing | Add `TraceContext` propagation to worker→aggregator messages | Demonstrates W3C traceparent injection/extraction |
| Demo app coroutines | Add a `CoroutineEchoActor` using `co_await` | Demonstrates coroutine actor pattern |
| Demo app lifecycle | Convert one Worker to `LifecycleActor` + `StatefulActor` | Demonstrates state machine transitions in `/actor show` |

### 1.2 Goals

1. Eliminate all "not yet implemented" and placeholder CLI responses.
2. Add CLI commands for every production subsystem with a public inspection API.
3. Extend the demo app to exercise ask/request-response, tracing, structured
   logging, coroutine actors, lifecycle state machine, and endpoint circuit
   breakers.
4. Provide meaningful per-command output that validates the framework is
   operating correctly — every command must show real runtime data.
5. Keep changes backward-compatible: existing actors, CLI commands, and test
   binaries must continue to work.

### 1.3 Non-Goals

- Adding commands for design/backlog features (ACK/NACK retry, durable outbox,
  cluster sharding, security, admin API).
- Implementing multi-node or remote-actor features in the demo.
- Adding the passivation/hibernation flow to the demo (requires storage backend).
- Refactoring the CLI command registration infrastructure.
- Adding new protobuf message types (extend existing ones where possible).

## 2. CLI Command Design — Stubs to Real

### 2.1 `/metrics show` — Metrics Snapshot

**Current state:** Prints `"metrics show — not yet implemented"`.

**Design:** The `MetricsActor` has a `MetricRegistry` with `snapshot()` and an
`Aggregator` that drains the ring buffer. The CLI command will:

1. Locate the `MetricsActor` via `ActorSystem::for_each_actor()` (filter by type
   name `"MetricsActor"`).
2. Send an `InspectStateRequest` with a new flag `include_metrics_snapshot=true`.
3. `MetricsActor` processes the inspect request by draining the ring buffer
   through the aggregator, taking a `MetricRegistry::snapshot()`, and
   serializing the counters/gauges/histograms into the reply's `state_blob`.

**Output format:**
```
Metrics Snapshot
────────────────
  Counters:
    hpactor_mailbox_enqueued_total        12,847
    hpactor_mailbox_dequeued_total        12,801
    hpactor_actor_spawned_total              10
    hpactor_delivery_failures_total          42
  Gauges:
    hpactor_actor_count_current              10
    hpactor_mailbox_depth_max                 8
  Histograms:
    hpactor_processing_latency_us
      p50=142.0  p99=892.0  max=5100.0
```

**Files to change:**
- `src/cli/commands/misc_commands.cpp` — replace stub with real implementation
- `include/hpactor/metrics/metrics_actor.hpp` — add inspect handler
- `src/metrics/metrics_actor.cpp` — implement inspect handler

**Framework API dependencies:** All exist. `MetricRegistry::snapshot()`,
`Aggregator::drain()`, and `OpenMetricsFormatter` are already wired.

### 2.2 `/topology show` — Topology Tree

**Current state:** Prints `"topology show — not yet implemented"`.

**Design:** The `TopologyModel` is built during `load_topology()` but discarded
after bootstrap. Two options:

- **Option A (preferred):** Add `ActorSystem::set_topology_model()` called by
  `BootstrapEngine`, and a `const TopologyModel* topology_model()` accessor.
  The model is immutable after bootstrap, so no lifetime issues.
- **Option B:** Walk the actor tree at runtime via `for_each_actor()` and
  reconstruct parent/child relationships from link/monitor edges.

Option A is preferred because it preserves the full topology including
dispatchers, resource specs, and the DAG structure. For the demo app which uses
programmatic spawn (not TOML), the topology is implicit — we show the runtime
actor tree reconstructed from spawn relationships.

**Output format:**
```
Topology
────────
  ActorSystem (4 scheduler threads, A2WS)
  ├── CliActor [system]
  ├── MetricsActor [system]
  ├── LogActor (StatefulActor<LogRingBuffer>)
  ├── ClockActor
  ├── AggregatorActor
  ├── SystemMonitorActor
  ├── HealthCheckActor
  ├── BroadcastActor
  ├── DlqDemoActor (circuit_breaker, quarantine)
  ├── WorkerActor worker-1 (rate_limit=100/s)
  ├── WorkerActor worker-2 (rate_limit=500/s)
  ├── WorkerActor worker-3 (circuit_breaker, quarantine)
  └── WorkerActor worker-4 (delivery_failures, quarantine)
```

**Files to change:**
- `src/cli/commands/misc_commands.cpp` — replace stub
- `include/hpactor/core/actor_system.hpp` — add `topology_model()` accessor (optional)
- `include/hpactor/config/topology_model.hpp` — ensure it's copyable/retainable

**Framework API dependencies:** `ActorSystem::for_each_actor()` +
`to_metadata()` covers the runtime tree. `TopologyModel` retention is a minor
addition.

### 2.3 `/ask pending` — List In-Flight Asks

**Current state:** Prints `"ask pending: not yet implemented"`.

**Design:** `AskManager` currently only exposes `pending_count()`. Add a
lightweight snapshot API mirroring `OutboundDeliveryTracker::snapshot()`:

```cpp
// New on AskManager
struct PendingAskSnapshot {
    uint64_t msg_id;
    ActorId requester_id;
    std::string target;        // actor address string
    uint64_t elapsed_ms;       // time since registration
    uint64_t deadline_ms;      // remaining deadline (0 = none)
};
std::vector<PendingAskSnapshot> snapshot() const;
```

The CLI command calls `system->ask_manager()->snapshot()` and renders the table.

**Output format:**
```
In-Flight Ask Requests (2 pending)
───────────────────────────────────
MsgID     Requester    Target       Elapsed   Deadline
─────     ─────────    ──────       ───────   ────────
0x0007    Actor-0x03   ClockActor   150ms     4850ms
0x0008    Actor-0x03   ClockActor    20ms     4980ms
```

**Files to change:**
- `include/hpactor/actor/ask_manager.hpp` — add `PendingAskSnapshot` struct +
  `snapshot()` method
- `src/actor/ask_manager.cpp` — implement `snapshot()`
- `src/cli/commands/ask_commands.cpp` — replace stub

**Framework API dependencies:** `AskManager::snapshot()` needs to be added
(trivial: iterate `pending_` map under mutex, copy fields).

### 2.4 `/ask cancel <msg_id>` — Cancel an Ask

**Current state:** Parses `--msg-id N`, prints `"not yet implemented"`.

**Design:** Add `AskManager::cancel(uint64_t msg_id)` that:
1. Looks up the message in `pending_`.
2. Resolves the handle with `errors::cancelled`.
3. Cancels the associated timeout timer.
4. Removes from `pending_`.

The CLI command already handles `--msg-id` parsing. Just wire it to the new API.

**Files to change:**
- `include/hpactor/actor/ask_manager.hpp` — add `cancel(uint64_t)`
- `src/actor/ask_manager.cpp` — implement `cancel()`
- `src/cli/commands/ask_commands.cpp` — replace stub

### 2.5 `/ask stats` — Ask Manager Statistics

**Current state:** Prints `"ask stats: not yet implemented"`.

**Design:** Add counters to `AskManager`:

```cpp
struct AskStats {
    uint64_t total_registered;
    uint64_t total_resolved;
    uint64_t total_timed_out;
    uint64_t total_cancelled;
    size_t   pending;
    uint64_t avg_latency_us;  // average time from register to resolve
};
AskStats stats() const;
```

**Files to change:**
- `include/hpactor/actor/ask_manager.hpp` — add `AskStats` + `stats()`
- `src/actor/ask_manager.cpp` — add atomic counters, implement `stats()`
- `src/cli/commands/ask_commands.cpp` — replace stub

### 2.6 `/system memory` — Memory Subsystem

**Current state:** Prints `"Use /metrics show for detailed memory stats"`.

**Design:** Query `MemoryRegionRegistry::instance().snapshot()` for each of the
6 region types (`kActor`, `kMessage`, `kCoroutine`, `kNetwork`, `kInternal`,
`kHibernate`). The registry is a process-wide singleton — no `ActorSystem`
dependency needed.

**Output format:**
```
Memory Regions
──────────────
Region        Active         Limit        Pressure    Allocs    Frees    Corruptions
──────        ──────         ─────        ────────    ──────    ─────    ───────────
kActor        1.2 MB         16 MB        Low          45,230    44,987   0
kMessage      384 KB          4 MB        Low         128,450   128,100   0
kCoroutine    64 KB           2 MB        Low           1,024     1,024   0
kNetwork      256 KB          8 MB        Low          12,340    12,100   0
kInternal     128 KB          2 MB        Low           8,760     8,740   0
kHibernate    0 KB            8 MB        Normal            0         0   0
```

**Files to change:**
- `src/cli/commands/system_commands.cpp` — replace `SystemMemoryCommand` stub

**Framework API dependencies:** All exist. `MemoryRegionRegistry::instance()`,
`snapshot(region)`, `to_string(RegionType)`.

### 2.7 `/system endpoints` — Endpoint Listing

**Current state:** Placeholder message.

**Design:** The framework has `EndpointOutboundQueue` and
`EndpointCircuitBreaker` per endpoint. These are managed by the transport
subsystem. Add an accessor to enumerate known endpoints:

```cpp
// New on ActorSystem (or TcpTransport)
struct EndpointInfo {
    EndPoint endpoint;
    size_t outbound_queue_depth;
    size_t outbound_queue_capacity;
    std::string circuit_state;    // "Closed" / "Open" / "HalfOpen"
    uint32_t circuit_trip_count;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};
std::vector<EndpointInfo> endpoint_snapshot() const;
```

For the demo app (which runs fully local), this will show the local loopback
endpoint. In a multi-node setup, it shows all connected peers.

**Output format:**
```
Endpoints (1 known)
───────────────────
Endpoint                  Queue       Circuit    Bytes Sent    Bytes Recv
────────                  ─────       ───────    ──────────    ──────────
127.0.0.1:0 (loopback)    0/1024      Closed     0             0
```

**Files to change:**
- `src/cli/commands/endpoint_commands.cpp` — replace all three stubs
- `include/hpactor/net/tcp_transport.hpp` — add `endpoint_snapshot()` (or on ActorSystem)

### 2.8 `/system endpoint/<ep>/show` — Endpoint Detail

**Design:** For a specific endpoint, show full detail: limits, depths,
pressure, circuit breaker state, bytes transferred, connection state.

### 2.9 `/system endpoint/<ep>/circuit/reset` — Reset Endpoint Circuit

**Design:** Call `EndpointCircuitBreaker::reset()` for the named endpoint.
Emit a structured log entry for the audit trail.

## 3. New CLI Commands

### 3.1 `/tracing status` — Tracing Subsystem Status

**Framework API:** `ActorSystem::trace_manager()` returns `TraceManager*`.
Public methods: `enabled()`, `config().sampling_rate`,
`config().ring_buffer_capacity`, `spans_dropped()`.

**Output format:**
```
Tracing Status
──────────────
  Enabled:          yes
  Sampling rate:    0.10 (10%)
  Ring buffer:      4096 spans
  Spans dropped:    0
  Exporter:         memory
```

**Files to change:**
- New file: `src/cli/commands/tracing_commands.cpp`

### 3.2 `/log level [<level>]` — Log Level Query/Set

**Framework API:** `LogManager` has `config()` accessor returning `LogConfig`
with level/category configuration. However, `LogManager` is NOT publicly
accessible from `ActorSystem` (it's `log_manager_` private member).

**Design:** Add `ActorSystem::log_manager()` accessor. The command:
- Without argument: shows current levels per category
- With argument: sets the default level (trace/debug/info/warn/error)

**Files to change:**
- `include/hpactor/core/actor_system.hpp` — add `log_manager()` accessor
- New file: `src/cli/commands/log_commands.cpp`

### 3.3 `/scheduler workers` — Worker Thread Status

**Framework API:** `IScheduler` has `worker_count()`. Worker-level stats
(steal count, idle time) are tracked internally in `HybridScheduler` but not
exposed via the interface.

**Design:** Add a `WorkerSnapshot` struct and `worker_snapshots()` to
`IScheduler`, implemented by `HybridScheduler`:

```cpp
struct WorkerSnapshot {
    uint16_t worker_index;
    uint64_t actors_executed;
    uint64_t steals_attempted;
    uint64_t steals_successful;
    uint64_t idle_spins;
    bool     is_idle;
};
std::vector<WorkerSnapshot> worker_snapshots() const;
```

**Output format:**
```
Scheduler Workers (4 threads, A2WS)
────────────────────────────────────
Worker  Executed   Steals (attempt/success)  Idle
──────  ────────   ────────────────────────  ────
0       12,450     340 / 12                  no
1       11,890     280 / 8                   no
2       12,100     310 / 15                  no
3       12,340     295 / 10                  no
```

**Files to change:**
- `include/hpactor/sched/scheduler_interfaces.hpp` — add `WorkerSnapshot` + virtual method
- `include/hpactor/sched/hybrid_scheduler.hpp` — implement
- New file: `src/cli/commands/scheduler_commands.cpp`

### 3.4 `/actor <id> links` — Actor Link/Monitor Graph

**Framework API:** `EventBasedActor` tracks linked and monitored actors
internally (for death propagation). These are not currently exposed.

**Design:** Extend `InspectStateRequest` with `include_links=true`. In the
actor's inspect handler, populate:
- `linked_actors` — bidirectional links from `link_to()`
- `monitored_actors` — one-way watches from `monitor()`
- `monitored_by` — actors watching this one

**Files to change:**
- `protos/hpactor/cli_messages.proto` — extend `InspectStateReply`
- `include/hpactor/actor/event_based_actor.hpp` — add links to inspect reply
- `src/cli/commands/actor_commands.cpp` — add `ActorLinksCommand`

### 3.5 `/actor <id> backpressure` — Backpressure Signal State

**Framework API:** `MPSCActorMailbox` tracks `PressureStateMachine` state and
`BackpressureSignalGate` emission. These are already serialized into
`MboxSnapshot` fields.

**Design:** Add a dedicated command that queries the mailbox snapshot and
renders the backpressure state with more detail than `/actor show`:

**Output format:**
```
Backpressure — Actor 0x0003 (WorkerActor worker-1)
──────────────────────────────────────────────────
  Pressure state:    Low
  Depth:             12/256 (4.7%)
  Byte utilization:  6.1 KB / 64 KB (9.5%)
  Signals emitted:   0
  Local consumers:   0 registered
  Remote consumers:  0 registered
  Last signal at:    never
```

**Files to change:**
- `src/cli/commands/actor_commands.cpp` — add `ActorBackpressureCommand`

## 4. Demo App Feature Gaps

### 4.1 Ask/Request-Response — `QueryActor`

Add a new actor that periodically sends `ask()` requests to `ClockActor`:

```cpp
class QueryActor : public EventBasedActor {
    // Every 2 seconds: context()->ask(clock_addr, TimeQuery, timeout)
    //   → RequestHandle<StreamBuffer>
    // On response: log elapsed time, update stats
    // On timeout: increment timeout counter
    // CLI visible via /actor <id> show → "queries_sent, responses, timeouts"
};
```

This exercises the full ask lifecycle: `AskManager::register_ask()`,
`on_response()`, `on_timeout()`, and `RequestHandle::get()`. Also provides
real data for `/ask pending`, `/ask cancel`, and `/ask stats`.

**ClockActor change:** Register an `on_request<TimeQueryTag, TimeReplyTag>`
handler (currently uses manual `context()->reply()` — either pattern works).

### 4.2 Distributed Tracing — Trace Propagation

Extend `WorkerActor` and `AggregatorActor` to propagate trace context:

1. In `WorkerActor::do_work()`, create a child span via
   `context()->trace_context()` before sending to the aggregator.
2. In `AggregatorActor`, extract the incoming trace context and create a
   server span wrapping the result processing.
3. `TraceManager` auto-exports completed spans to the memory exporter.

Visible via the new `/tracing status` command (spans processed count).

### 4.3 Structured Logging — Use `LogManager`

Replace `LogActor`'s raw ring buffer with the framework's `LogManager`:

1. In `WorkerActor`, replace manual `LogEntryTag` sends with
   `system().log_manager()->logger().info("Worker-{}: {} tasks", id, count)`.
2. `LogActor` becomes a log consumer that formats recent entries for CLI
   inspection (or is removed entirely — the `LogManager` + `StderrSink` already
   produces output).

This makes the new `/log level` command meaningful.

### 4.4 Coroutine Actor — `CoroutineEchoActor`

Add an actor that uses `co_await` for message receive:

```cpp
class CoroutineEchoActor : public EventBasedActor {
    // Uses coroutine behavior: co_await mailbox_awaiter
    // Receives any message, echoes back with "Echo: " prefix
    // Demonstrates co_await suspend/resume in scheduler
};
```

### 4.5 Lifecycle State Machine

Convert `WorkerActor` (worker-3) to also inherit from `LifecycleActor`:

```cpp
class WorkerActor : public EventBasedActor, public LifecycleActor {
    // Lifecycle states visible via /actor <id> show:
    //   Created → Starting → Running → Stopping → Stopped
    // Message gate rejects during non-Active states
};
```

### 4.6 Typed Actor

Add a `TypedCalculatorActor` using `TypedEventBasedActor<Add, Subtract>`:

```cpp
using CalculatorSig = TypedEventBasedActor<
    reacts_to<AddRequest, AddResponse>,
    reacts_to<SubtractRequest, SubtractResponse>
>;
```

This demonstrates static type-safe message handling alongside the
dynamic-protobuf actors.

## 5. Implementation Plan

### Phase 1: CLI Stub Elimination (low risk, no new framework APIs)

| # | Task | Files | Effort |
|---|------|-------|--------|
| 1.1 | `/system memory` → real | `system_commands.cpp` | S |
| 1.2 | `/metrics show` → real | `misc_commands.cpp`, `metrics_actor.*` | M |
| 1.3 | `/topology show` → real | `misc_commands.cpp`, `actor_system.hpp` | S |
| 1.4 | `/system endpoints` → real | `endpoint_commands.cpp` | M |
| 1.5 | `/system endpoint/<ep>/show` → real | `endpoint_commands.cpp` | M |
| 1.6 | `/system endpoint/<ep>/circuit/reset` → real | `endpoint_commands.cpp` | S |

### Phase 2: AskManager Inspection API + CLI

| # | Task | Files | Effort |
|---|------|-------|--------|
| 2.1 | Add `AskManager::snapshot()` | `ask_manager.hpp`, `ask_manager.cpp` | M |
| 2.2 | Add `AskManager::cancel()` | `ask_manager.hpp`, `ask_manager.cpp` | S |
| 2.3 | Add `AskManager::stats()` | `ask_manager.hpp`, `ask_manager.cpp` | M |
| 2.4 | Wire `/ask pending/cancel/stats` | `ask_commands.cpp` | S |

### Phase 3: New CLI Commands

| # | Task | Files | Effort |
|---|------|-------|--------|
| 3.1 | `/tracing status` | New `tracing_commands.cpp` | S |
| 3.2 | `/log level` | `actor_system.hpp`, new `log_commands.cpp` | S |
| 3.3 | `/scheduler workers` | `scheduler_interfaces.hpp`, `hybrid_scheduler.*`, new `scheduler_commands.cpp` | M |
| 3.4 | `/actor <id> links` | `cli_messages.proto`, `actor_commands.cpp` | M |
| 3.5 | `/actor <id> backpressure` | `actor_commands.cpp` | S |

### Phase 4: Demo App Feature Expansion

| # | Task | Files | Effort |
|---|------|-------|--------|
| 4.1 | `QueryActor` (ask demo) | New `query_actor.hpp`, `15_cli_demo.cpp` | M |
| 4.2 | Trace propagation in workers | `worker_actor.hpp`, `aggregator_actor.hpp` | S |
| 4.3 | Structured logging integration | `worker_actor.hpp`, `log_actor.hpp` | M |
| 4.4 | `CoroutineEchoActor` | New `coroutine_echo_actor.hpp`, `15_cli_demo.cpp` | M |
| 4.5 | Lifecycle state machine on Worker-3 | `worker_actor.hpp` | M |
| 4.6 | `TypedCalculatorActor` | New `typed_calculator_actor.hpp` | M |

### Phase 5: Test Coverage

| # | Task | Files | Effort |
|---|------|-------|--------|
| 5.1 | CLI command unit tests | `tests/unit/cli/test_*_commands.cpp` | M |
| 5.2 | AskManager snapshot/cancel/stats tests | `tests/unit/actor/test_ask_manager.cpp` | S |
| 5.3 | Demo app integration test | `tests/system/test_cli_demo_missing.cpp` | M |

### Test Design Constraints (per CLAUDE.md)

- CLI command tests use `scheduler_threads = 0` to avoid races observing state.
- `AskManager` tests inject requests directly, bypassing the scheduler.
- No timing assumptions — use condition-based polling with 5s+ timeouts for
  tests that need the scheduler.
- `MemoryRegionRegistry` tests query snapshots directly; no thread coordination.

## 6. Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| `AskManager` snapshot/cancel API changes break existing callers | Low | Additive API; existing `register_ask()`/`on_response()`/`on_timeout()` unchanged |
| `TopologyModel` retention increases memory | Negligible | Model is small (<1KB for demo); only retained when loaded via TOML |
| `IScheduler` interface change breaks alternative scheduler backends | Low | Only `HybridScheduler` exists; virtual default returns empty vector |
| Lifecycle mixin on WorkerActor breaks existing behavior | Medium | Make opt-in via `WorkerConfig::lifecycle_enabled` flag |
| Proto changes for `include_links` break wire compatibility | Low | Additive field; protobuf backward-compatible by design |

## 7. Observability

Each new CLI command emits no side effects — they are read-only inspection
operations. The `/system endpoint/<ep>/circuit/reset` command emits a
structured log entry (`LogCategory::kCli`, `LogLevel::kWarn`) for audit.

New demo app actors emit metrics events through the existing ring buffer:
- `QueryActor`: `kAskRegistered`, `kAskResolved`, `kAskTimeout`
- `CoroutineEchoActor`: `kActorSuspended`, `kActorResumed`

## 8. Files Summary

### New files
- `src/cli/commands/tracing_commands.cpp`
- `src/cli/commands/log_commands.cpp`
- `src/cli/commands/scheduler_commands.cpp`
- `apps/cli_demo/actors/query_actor.hpp`
- `apps/cli_demo/actors/coroutine_echo_actor.hpp`
- `apps/cli_demo/actors/typed_calculator_actor.hpp`
- `tests/unit/cli/test_tracing_commands.cpp`
- `tests/unit/cli/test_log_commands.cpp`
- `tests/unit/cli/test_scheduler_commands.cpp`
- `tests/unit/cli/test_metrics_commands.cpp`
- `tests/system/test_cli_demo_missing.cpp`

### Modified files
- `src/cli/commands/misc_commands.cpp` — `/metrics show`, `/topology show`
- `src/cli/commands/system_commands.cpp` — `/system memory`
- `src/cli/commands/endpoint_commands.cpp` — all three endpoint commands
- `src/cli/commands/ask_commands.cpp` — `/ask pending/cancel/stats`
- `src/cli/commands/actor_commands.cpp` — `/actor <id> links`, `/actor <id> backpressure`
- `include/hpactor/actor/ask_manager.hpp` — `snapshot()`, `cancel()`, `stats()`
- `src/actor/ask_manager.cpp` — implementations
- `include/hpactor/core/actor_system.hpp` — `log_manager()`, `topology_model()` accessors
- `include/hpactor/sched/scheduler_interfaces.hpp` — `WorkerSnapshot`, `worker_snapshots()`
- `include/hpactor/sched/hybrid_scheduler.hpp` — implement `worker_snapshots()`
- `include/hpactor/metrics/metrics_actor.hpp` — inspect handler
- `src/metrics/metrics_actor.cpp` — inspect handler implementation
- `protos/hpactor/cli_messages.proto` — extend `InspectStateReply`
- `apps/cli_demo/15_cli_demo.cpp` — spawn new actors, wire tracing/logging
- `apps/cli_demo/actors/worker_actor.hpp` — trace propagation, lifecycle opt-in
- `apps/cli_demo/actors/aggregator_actor.hpp` — trace extraction
- `apps/cli_demo/messages.hpp` — new TypeTags for QueryActor, Calculator
