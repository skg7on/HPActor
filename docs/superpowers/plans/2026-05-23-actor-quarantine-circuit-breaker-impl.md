# Actor Quarantine & Circuit Breaker Implementation Plan

## Summary

Add actor quarantine and circuit breaker to HPActor per the design spec in
`docs/architecture/production/actor-quarantine-circuit-breaker-design.md`.

This plan implements ACT-004 from the production architecture backlog.
Estimated: 6 phases, ~1500 lines of new code across headers, source, and tests.

## Phase 1: Core Types & Configuration (Foundation)

**Goal:** Define all new types, enums, and config structures. No runtime behavior yet.

### 1.1 QuarantineReason enum

**File:** `include/hpactor/actor/quarantine_reason.hpp` (new)

```cpp
enum class QuarantineReason : uint8_t {
    SupervisionEscalation = 0,
    CircuitBreakerTrip = 1,
    MailboxPressure = 2,
    OperatorAction = 3,
    RecoveryFailure = 4,
};
const char* to_string(QuarantineReason reason) noexcept;
```

### 1.2 QuarantinePolicy struct

**File:** `include/hpactor/actor/quarantine_policy.hpp` (new)

- `bool enabled = false` -- master switch
- `bool escalate_on_max_restarts = true`
- `uint32_t failure_rate_threshold = 0` -- 0 = disabled
- `uint32_t timeout_rate_threshold = 0`
- `float mailbox_pressure_threshold = 0.0f`
- `std::chrono::milliseconds cooldown_period{30'000}`
- `std::chrono::milliseconds observation_window{10'000}`
- `uint32_t max_circuit_trips = 3`

### 1.3 CircuitBreakerState enum + CircuitBreakerTracker struct

**File:** `include/hpactor/actor/circuit_breaker.hpp` (new)

- `enum class CircuitBreakerState : uint8_t { kClosed, kOpen, kHalfOpen }`
- `struct CircuitBreakerTracker` with state, trip_count, timestamps, EMA rates
- `const char* to_string(CircuitBreakerState state) noexcept`

### 1.4 FailureRateTracker

**File:** `include/hpactor/actor/failure_rate_tracker.hpp` (new)

- Ring buffer of time-bucketed failure and timeout counts
- `record_failure()`, `record_timeout()`, `advance_buckets(now)`
- `failure_rate()` and `timeout_rate()` in events/sec over the window
- Template parameter for bucket count (default 10)

### 1.5 LifecycleState Extension

**File:** `include/hpactor/actor/lifecycle_state.hpp` (modify)

- Add `kQuarantined = 7` to `LifecycleState` enum
- Add `StateDef` entry for `kQuarantined`
- Update `kActive`, `kFailed`, `kRecovering` transitions to include `kQuarantined`
- Update `static_assert` from 7 to 8 states

### 1.6 Supervision Directive Extension

**File:** `include/hpactor/supervision/supervision.hpp` (modify)

- Add `Quarantine` to `SupervisionDirective` enum
- Add `QuarantinePolicy quarantine_policy` to `SupervisionPolicy` struct

### 1.7 TOML Config Parser

**File:** `src/config/parsers/quarantine_parser.cpp` (new)

- Self-registering `TomlSystemParserRegistration` for `[system.quarantine]`
- Parse global defaults: `default_enabled`, `default_cooldown_period_ms`,
  `default_observation_window_ms`

### 1.8 Source file for new types

**File:** `src/actor/quarantine_reason.cpp` (new)

- `to_string(QuarantineReason)` implementation

### Files touched in Phase 1

| File | Action |
|---|---|
| `include/hpactor/actor/quarantine_reason.hpp` | Create |
| `include/hpactor/actor/quarantine_policy.hpp` | Create |
| `include/hpactor/actor/circuit_breaker.hpp` | Create |
| `include/hpactor/actor/failure_rate_tracker.hpp` | Create |
| `include/hpactor/actor/lifecycle_state.hpp` | Modify -- add kQuarantined, update transitions |
| `include/hpactor/supervision/supervision.hpp` | Modify -- add Quarantine directive, QuarantinePolicy field |
| `src/actor/quarantine_reason.cpp` | Create |
| `src/config/parsers/quarantine_parser.cpp` | Create |
| `src/CMakeLists.txt` | Modify -- add new source files |
| `tests/unit/actor/test_quarantine_reason.cpp` | Create -- enum to_string, completeness |
| `tests/unit/actor/test_circuit_breaker_tracker.cpp` | Create -- state transitions, EMA, trip count |
| `tests/unit/actor/test_failure_rate_tracker.cpp` | Create -- bucket advance, rate calculation |
| `tests/unit/actor/test_lifecycle_state.cpp` | Modify -- add kQuarantined transition tests |
| `tests/CMakeLists.txt` | Modify -- add new test targets |

**Phase 1 acceptance:** Types compile, enum tests pass, LifecycleState has 8
entries with correct transitions, config parser round-trips.

---

## Phase 2: Quarantine Runtime Integration

**Goal:** Message gating for quarantined actors. Supervision escalation. No
circuit breaker yet.

### 2.1 LifecycleActor Quarantine Methods

**File:** `include/hpactor/actor/lifecycle_actor.hpp` (modify)
**File:** `src/actor/lifecycle_actor.cpp` (if exists, or add source)

- `result<void> transition_to_quarantined(QuarantineReason reason)`
- `result<void> transition_from_quarantined()`
- `bool is_quarantined() const noexcept`
- `QuarantineReason quarantine_reason() const noexcept`
- Store `QuarantineReason` and `Clock::time_point quarantined_at`

### 2.2 EventBasedActor Quarantine Gate

**File:** `src/actor/event_based_actor.cpp` (modify)

In `receive(TypedMessage& msg)`:
- Before user message dispatch, check `lifecycle_state_ == kQuarantined`
- System messages (LinkMsg, UnlinkMsg, DownMsg, InspectStateRequest, KillRequest,
  Stop): dispatch normally
- User messages: build `FailureEnvelope`, return to sender, log warning, emit
  metric event, return early

### 2.3 try_deliver_local Quarantine Check

**File:** `src/actor/actor_system.cpp` (modify)

In `try_deliver_local()`:
- After actor lookup, before mailbox injection
- If actor is quarantined and message is not a system message:
  - Build `FailureEnvelope(Quarantined, source=Supervision)`
  - Emit `kDeliveryFailure` metric with quarantine detail
  - Return early

### 2.4 Supervision Quarantine Escalation

**File:** `src/supervision/supervisor_actor.cpp` (modify)

In the restart-limit-exceeded path:
- Check `quarantine_policy_.escalate_on_max_restarts`
- If true: call `child->transition_to_quarantined(SupervisionEscalation)`
- If false: existing Stop behavior

### 2.5 DownMsg on Quarantine

**File:** `src/actor/event_based_actor.cpp` (modify)

When an actor transitions to `kQuarantined`:
- Send `DownMsg` to all linked and monitored actors
- DownMsg includes quarantine reason in detail

### Files touched in Phase 2

| File | Action |
|---|---|
| `include/hpactor/actor/lifecycle_actor.hpp` | Modify -- add quarantine methods |
| `src/actor/event_based_actor.cpp` | Modify -- quarantine gate in receive() |
| `src/actor/actor_system.cpp` | Modify -- quarantine check in try_deliver_local() |
| `src/supervision/supervisor_actor.cpp` | Modify -- quarantine escalation |
| `src/actor/local_actor.cpp` | Modify -- DownMsg on quarantine transition |
| `tests/integration/actor/test_quarantine_message_gate.cpp` | Create |
| `tests/integration/actor/test_quarantine_supervision.cpp` | Create |
| `tests/integration/supervision/test_supervision_quarantine.cpp` | Create |

**Phase 2 acceptance:** Quarantined actors reject user messages with
`FailureEnvelope(Quarantined)`, accept system messages, supervision escalation
works, linked actors receive DownMsg.

---

## Phase 3: Circuit Breaker Runtime

**Goal:** Per-actor circuit breaker state machine integrated with message
delivery. Failure rate tracking.

### 3.1 CircuitBreakerTracker on Actor

**File:** `include/hpactor/actor/event_based_actor.hpp` (modify)

- Add optional `CircuitBreakerTracker` member (gated on `QuarantinePolicy::enabled`)
- Add `FailureRateTracker` member (same gate)

### 3.2 Circuit Breaker Evaluation in Delivery Path

**File:** `src/actor/actor_system.cpp` (modify)

In `try_deliver_local()`, after actor lookup, before mailbox injection:
- If actor has circuit breaker in `kOpen` state:
  - Check cooldown expiry -> transition to `kHalfOpen` if expired
  - If still in cooldown: build `FailureEnvelope(CircuitOpen)`, return
- If actor has circuit breaker in `kHalfOpen`:
  - If probe already in flight: build `FailureEnvelope(CircuitOpen)`, return
  - If no probe: mark probe in flight, allow message through

### 3.3 Failure Recording

**File:** `src/actor/event_based_actor.cpp` (modify)

In message processing completion path:
- On success: if circuit breaker is `kHalfOpen`, transition to `kClosed`,
  reset trip count, emit metric
- On failure/timeout: record in `FailureRateTracker`, update EMA, check
  thresholds; trip circuit if exceeded
- If `kOpen` repeated `max_circuit_trips` times: escalate to quarantine

### Files touched in Phase 3

| File | Action |
|---|---|
| `include/hpactor/actor/event_based_actor.hpp` | Modify -- add CircuitBreakerTracker + FailureRateTracker |
| `src/actor/actor_system.cpp` | Modify -- circuit breaker evaluation in delivery |
| `src/actor/event_based_actor.cpp` | Modify -- failure recording, circuit state machine |
| `tests/integration/actor/test_circuit_breaker_delivery.cpp` | Create |
| `tests/integration/actor/test_circuit_breaker_state_machine.cpp` | Create |
| `tests/integration/actor/test_quarantine_circuit_escalation.cpp` | Create |

**Phase 3 acceptance:** Circuit breaker trips on threshold, cooldown works,
half-open probe succeeds/fails correctly, repeated trips escalate to quarantine.

---

## Phase 4: Metrics & Logging

**Goal:** Observability surface for quarantine and circuit breaker.

### 4.1 Metric Event Types

**File:** `include/hpactor/metrics/metrics_event.hpp` (modify)

Add to `MetricEventType`:
- `kActorQuarantined = 21`
- `kActorUnquarantined = 22`
- `kCircuitStateChange = 23`

### 4.2 Metric Registry Integration

**File:** `include/hpactor/metrics/metrics_registry.hpp` (modify)
**File:** `src/metrics/metrics_registry.cpp` (modify)

New counters and gauges:
- `hpactor_actor_quarantine_total` (counter, label: `reason`)
- `hpactor_actor_unquarantine_total` (counter)
- `hpactor_actor_circuit_state` (gauge, labels: `actor_id`, `state`)
- `hpactor_actor_circuit_trips_total` (counter, labels: `actor_id`)

### 4.3 Metrics Aggregator Updates

**File:** `src/metrics/metrics_aggregator.cpp` (modify)

Handle new event types in the aggregator dispatch.

### 4.4 Structured Logging

**File:** `src/actor/event_based_actor.cpp` (modify)
**File:** `src/supervision/supervisor_actor.cpp` (modify)

Log events (via existing `LogManager`/`Logger`):
- `actor_quarantined` at WARN
- `actor_unquarantined` at INFO
- `circuit_state_change` at WARN

### Files touched in Phase 4

| File | Action |
|---|---|
| `include/hpactor/metrics/metrics_event.hpp` | Modify -- new event types |
| `include/hpactor/metrics/metrics_registry.hpp` | Modify -- new metric declarations |
| `src/metrics/metrics_registry.cpp` | Modify -- register new metrics |
| `src/metrics/metrics_aggregator.cpp` | Modify -- dispatch new events |
| `src/actor/event_based_actor.cpp` | Modify -- emit metric events + log on quarantine |
| `tests/unit/metrics/test_quarantine_metrics.cpp` | Create |

**Phase 4 acceptance:** Metrics emitted on state transitions, log entries
written with correct correlation IDs.

---

## Phase 5: CLI & Operations Surface

**Goal:** Operator commands for inspect, manual quarantine, and override.

### 5.1 CLI Commands

**File:** `src/cli/commands/actor_commands.cpp` (modify)

New commands:
```
/actor <id> quarantine [--reason <text>]
/actor <id> unquarantine
/actor <id> circuit
/actor list --quarantined
```

### 5.2 InspectState Protocol Extension

**File:** protobuf definition for cli (modify)

Add fields to `InspectStateRequest`:
- `bool include_quarantine_info = N`
- `bool include_circuit_breaker_info = N`
- `optional QuarantineAction quarantine_action = N`
- `optional string quarantine_reason_text = N`

Add fields to `InspectStateReply`:
- `optional QuarantineInfo quarantine_info = N`
- `optional CircuitBreakerInfo circuit_breaker_info = N`

### 5.3 ActorSystem Registry Query

**File:** `include/hpactor/core/actor_system.hpp` (modify)
**File:** `src/actor/actor_system.cpp` (modify)

- `std::vector<ActorId> get_quarantined_actors() const`

### Files touched in Phase 5

| File | Action |
|---|---|
| `src/cli/commands/actor_commands.cpp` | Modify -- new quarantine/circuit commands |
| Protobuf: `hpactor/cli/cli.proto` | Modify -- InspectState extension |
| `include/hpactor/core/actor_system.hpp` | Modify -- get_quarantined_actors() |
| `src/actor/actor_system.cpp` | Modify -- registry query |
| `tests/integration/cli/test_cli_quarantine.cpp` | Create |
| `tests/unit/cli/test_quarantine_commands.cpp` | Create |

**Phase 5 acceptance:** CLI commands work end-to-end, operator can inspect,
quarantine, and unquarantine actors.

---

## Phase 6: TOML Config & Bootstrap Integration

**Goal:** End-to-end config-driven quarantine policy on spawned actors.

### 6.1 QuarantineConfigParser

**File:** `src/config/parsers/quarantine_parser.cpp` (already created in Phase 1, finalize)

- Parse `[system.quarantine]` defaults
- Parse per-actor `[actors.<name>.quarantine]` settings
- Merge defaults with per-actor overrides

### 6.2 BootstrapEngine Integration

**File:** `src/config/bootstrap_engine.cpp` (modify)

During `spawn_configured()`:
- If `ActorDef` has quarantine config, call `actor->configure_quarantine(policy)`

### 6.3 ConfigurableActor Extension

**File:** `include/hpactor/config/actor_factory.hpp` (modify)

- Add `virtual void configure_quarantine(const QuarantinePolicy& policy)` to
  `ConfigurableActor` concept (default no-op)

### 6.4 End-to-End Test

**File:** `tests/integration/config/test_quarantine_topology.cpp` (create)

- TOML topology with per-actor quarantine config
- Spawn actors, trigger supervision escalation, verify quarantine
- Verify circuit breaker trips from config thresholds

### Files touched in Phase 6

| File | Action |
|---|---|
| `src/config/parsers/quarantine_parser.cpp` | Finalize |
| `src/config/bootstrap_engine.cpp` | Modify -- wire quarantine policy on spawn |
| `include/hpactor/config/actor_factory.hpp` | Modify -- configure_quarantine interface |
| `tests/integration/config/test_quarantine_topology.cpp` | Create |

**Phase 6 acceptance:** Full config -> spawn -> quarantine lifecycle works
end-to-end from TOML.

---

## Phase Dependencies

```
Phase 1 (Types + Config)
    |
    v
Phase 2 (Quarantine Runtime) ------> Phase 4 (Metrics & Logging)
    |                                      |
    v                                      |
Phase 3 (Circuit Breaker) ---------------->|
    |                                      |
    v                                      v
Phase 5 (CLI & Operations) <---------------+
    |
    v
Phase 6 (TOML Config & Bootstrap)
```

Phases 2 and 3 can be partially overlapped (circuit breaker needs quarantine
foundation but is independently testable). Phase 4 can begin once metrics
event points are identified in Phases 2-3. Phase 5 needs Phase 2 quarantine
working. Phase 6 is final integration.

## Test Plan Summary

| Phase | Test File | Focus |
|---|---|---|
| 1 | `test_quarantine_reason` | Enum values, to_string |
| 1 | `test_circuit_breaker_tracker` | State transitions, EMA, trip counting |
| 1 | `test_failure_rate_tracker` | Bucket advancement, rate calculation, edge cases |
| 1 | `test_lifecycle_state` (extend) | kQuarantined transitions, accept/reject flags |
| 2 | `test_quarantine_message_gate` | User msgs rejected, system msgs pass, FailureEnvelope content |
| 2 | `test_quarantine_supervision` | Escalation from max_restarts to quarantine |
| 2 | `test_supervision_quarantine` | Supervisor with quarantine policy, DownMsg propagation |
| 3 | `test_circuit_breaker_delivery` | Messages gated by circuit state |
| 3 | `test_circuit_breaker_state_machine` | Closed->Open->HalfOpen->Closed cycle |
| 3 | `test_quarantine_circuit_escalation` | Repeated trips -> quarantine |
| 4 | `test_quarantine_metrics` | Metric events emitted, counters increment |
| 5 | `test_cli_quarantine` | CLI commands end-to-end |
| 5 | `test_quarantine_commands` | Command tree integration |
| 6 | `test_quarantine_topology` | TOML config -> spawn -> quarantine lifecycle |

## Risk Register

| Risk | Mitigation |
|---|---|
| Quarantine state adds complexity to existing lifecycle transitions | Add `kQuarantined` as terminal sink (only -> kStopped), minimize new transition edges |
| Circuit breaker rate tracking overhead on hot path | Bucket-based EMA amortized per window slice (~1 eval/sec); disabled when policy is off |
| Supervision restart counting already complex | Quarantine is additive -- supervisor path only branches at "max exceeded" decision point |
| Protobuf InspectState extension breaks wire compatibility | Use new optional fields with safe defaults; existing clients ignore unknown fields |
| Quarantined actor memory not reclaimed | Policy choice: quarantine preserves state for inspection; supervisor can still force Stop -> reclaim |

## Rollback Plan

If `QuarantinePolicy::enabled` defaults to `false`, all new code paths are
gated behind an atomic boolean check. Rolling back:
1. Set `quarantine.enabled = false` globally in TOML (runtime) or
2. Revert the merge commit (source)

Existing behavior is preserved exactly when quarantine is disabled.
