# ACT-005: Circuit Breaker Policy — Detailed Implementation Spec

## 1. Executive Summary

Issue #10 ([ACT-005](https://github.com/skg7on/HPActor/issues/10)) requires a circuit
breaker policy for actors that fail or time out under load. The high-level architecture
design exists in `docs/architecture/production/actor-quarantine-circuit-breaker-design.md`,
and significant infrastructure is already built. This spec audits the current state,
identifies the precise gaps, and defines the remaining implementation in test-driven phases.

**Key finding:** The circuit breaker tracker, failure-rate tracker, quarantine policy,
lifecycle state, failure reason codes, supervision directive, config parser, and
post-processing result recording are all implemented. The critical gaps are:

1. **Pre-admission circuit breaker gating** — `try_deliver_local()` does not check
   circuit breaker state before injecting messages into the mailbox.
2. **Cooldown / Open→HalfOpen transition** — the circuit opens on failure but never
   transitions back to HalfOpen on cooldown expiry (the admission path should check this).
3. **HalfOpen probe admission control** — only one probe message should pass through
   in HalfOpen state.
4. **Integration/system tests** — no tests exercise the end-to-end circuit breaker
   lifecycle (trip → cooldown → probe → close or re-open → quarantine).

## 2. Current State Audit

### 2.1 Implemented and Verified

| Layer | File | Status |
|-------|------|--------|
| `CircuitBreakerState` enum (Closed/Open/HalfOpen) | `include/hpactor/actor/circuit_breaker.hpp` | Done |
| `CircuitBreakerTracker` struct (state, trip_count, opened_at, failure_ema, probe flag) | Same | Done |
| `FailureRateTracker` struct (10-bucket sliding window, failure_rate(), timeout_rate()) | `include/hpactor/actor/failure_rate_tracker.hpp` | Done |
| `QuarantinePolicy` struct (enabled, thresholds, cooldown, observation window, max_trips) | `include/hpactor/actor/quarantine_policy.hpp` | Done |
| `QuarantineReason` enum (SupervisionEscalation, CircuitBreakerTrip, etc.) | `include/hpactor/actor/quarantine_reason.hpp` | Done |
| `LifecycleState::kQuarantined` + state machine transitions | `include/hpactor/actor/lifecycle_state.hpp` | Done |
| `FailureReason::Quarantined` (value 12) | `include/hpactor/types/failure_reason.hpp` | Done |
| `FailureReason::CircuitOpen` (value 13) | Same | Done |
| `FailureSource::Supervision` | Same | Done |
| `SupervisionDirective::Quarantine` | `include/hpactor/supervision/supervision.hpp` | Done |
| `SupervisionPolicy::quarantine` field | Same | Done |
| `QuarantineConfigParser` (self-registering TOML parser for `[system.quarantine]`) | `src/config/parsers/quarantine_parser.cpp` | Done |
| `EventBasedActor::configure_quarantine()` — initializes tracker from policy | `src/actor/event_based_actor.cpp:65` | Done |
| `EventBasedActor::receive()` — quarantine message gate (rejects user msgs, emits FailureEnvelope + metric) | `src/actor/event_based_actor.cpp:156-188` | Done |
| `EventBasedActor::receive()` — records circuit breaker result per message processing | `src/actor/event_based_actor.cpp:291-319` | Done |
| `EventBasedActor::record_circuit_breaker_result()` — EMA update, threshold trip, HalfOpen→Closed, quarantine escalation | `src/actor/event_based_actor.cpp:608-657` | Done |
| Quarantine applied during spawn from actor def config | `src/actor/actor_system.cpp:1065-1072` | Done |
| Unit tests: `CircuitBreakerTracker` | `tests/unit/actor/test_circuit_breaker_tracker.cpp` | Done |
| Unit tests: `FailureRateTracker` | `tests/unit/actor/test_failure_rate_tracker.cpp` | Done |
| Unit tests: `QuarantineReason` | `tests/unit/actor/test_quarantine_reason.cpp` | Done |

### 2.2 NOT Yet Implemented (Gaps)

| Gap | Description | Priority |
|-----|-------------|----------|
| **G1: Pre-admission circuit breaker gate** | `try_deliver_local()` in `actor_system.cpp` does not check `CircuitBreakerTracker::state` before injecting into the mailbox. Messages flow through even when the circuit is open. | P0 |
| **G2: Cooldown expiry → HalfOpen transition** | The `opened_at` timestamp is set on trip, but no code checks elapsed time against `cooldown_period` to transition Open→HalfOpen. This check belongs at admission time in `try_deliver_local()`. | P0 |
| **G3: HalfOpen probe admission control** | When state is HalfOpen, only ONE message should be admitted (probe). `half_open_probe_in_flight` is set but never checked at admission time. | P0 |
| **G4: FailureEnvelope(CircuitOpen) return on admission** | When the circuit is open, `try_deliver_local()` must return a failure with `FailureReason::CircuitOpen` instead of admitting the message. | P0 |
| **G5: Timeout rate integrated into circuit trip** | `FailureRateTracker` tracks both failures and timeouts, but `record_circuit_breaker_result()` only checks `failure_rate_threshold`. Timeout-rate tripping is missing. | P1 |
| **G6: Mailbox pressure integration** | `mailbox_pressure_threshold` in `QuarantinePolicy` has no plumbing to the mailbox pressure signal. | P2 |
| **G7: CLI `/actor <id> circuit` command** | No CLI command to inspect circuit breaker state. | P1 |
| **G8: CLI `/actor <id> quarantine` / `unquarantine`** | No CLI commands for manual quarantine control. | P2 |
| **G9: Supervision escalation wiring** | `SupervisionPolicy::quarantine` field exists but `SupervisorActor::handle_child_down()` does not yet escalate to quarantine when `escalate_on_max_restarts` is set. | P1 |
| **G10: Circuit-state metric** | No `hpactor_actor_circuit_state` gauge metric emitted on state transitions. | P1 |
| **G11: Integration/system tests** | No tests exercise the full circuit breaker lifecycle end-to-end. | P0 |

## 3. Gap Detail & Implementation Design

### 3.1 G1–G4: Pre-Admission Circuit Breaker Gate in `try_deliver_local()`

**File:** `src/actor/actor_system.cpp`, function `try_deliver_local()` (line ~838)

**Design:** Before the existing duplicate/expired/mailbox-full checks, insert a
circuit breaker admission check. The check accesses the target actor's
`CircuitBreakerTracker` and `QuarantinePolicy` through the actor registry.

```
try_deliver_local(target, msg, ...):
    ┌─ NEW: circuit breaker admission gate ─┐
    │ actor = registry_->lookup(target)      │
    │ if actor has circuit breaker enabled:   │
    │   cb = actor->circuit_breaker()         │
    │   policy = actor->quarantine_policy()   │
    │                                         │
    │   if cb->state == kOpen:                │
    │     if now - cb->opened_at >= cooldown: │
    │       cb->state = kHalfOpen             │
    │       cb->half_open_probe_in_flight = true │
    │       goto ADMIT                         │
    │     else:                                │
    │       return FailureEnvelope(CircuitOpen) │  ← G4
    │                                         │
    │   if cb->state == kHalfOpen:            │
    │     if cb->half_open_probe_in_flight:    │
    │       return FailureEnvelope(CircuitOpen) │ ← G3
    │     // else allow through (probe already in flight) │
    │                                         │
    │ ADMIT: continue to existing checks      │
    └─────────────────────────────────────────┘
    → existing duplicate check
    → existing expired check
    → mailbox->try_push()
```

**Concurrency note:** `CircuitBreakerTracker` is single-writer (the owning actor's
scheduler thread). However, `try_deliver_local()` can be called from ANY thread
(remote senders, timer callbacks, other actors). The read of `state` and
`half_open_probe_in_flight` are non-atomic reads. The circuit breaker state is
written only by the scheduler thread (in `record_circuit_breaker_result()`), so
there is a narrow window where:
- A sender reads `kClosed`, admits a message
- The actor's scheduler thread simultaneously trips to `kOpen`
- The message that was admitted still gets processed

This is acceptable — the circuit breaker is not a transactional guarantee, it's a
rate-limiting protection. At most one extra message slips through after a trip,
which is the standard circuit breaker semantics (the "last message before trip" is
already in-flight in the mailbox).

**Return type consideration:** `try_deliver_local()` returns `mailbox::EnqueueResult`,
which does not have a `FailureReason::CircuitOpen` code in `EnqueueResultCode`. We
have two options:

**Option A (preferred):** Add `CircuitOpen` to `EnqueueResultCode` and handle it
in the caller. The `EnqueueResult` already has the full metadata to carry the
circuit breaker state.

**Option B:** Return `EnqueueResultCode::Rejected` with a detail string.

Option A is preferred because it gives callers precise information for retry decisions.

**Implementation detail — new `EnqueueResultCode` value:**

```cpp
// In mailbox/mailbox_policy.hpp, add:
CircuitOpen = 10,  ///< Circuit breaker is open — message rejected at admission.
```

**Implementation detail — actor lookup in `try_deliver_local()`:**

The function currently does `auto* mailbox = get_mailbox(target)` which returns
nullptr if the actor doesn't exist. We need to also get the actor itself to check
circuit breaker state. The `ActorRegistry` lookup can be done through
`AbstractActor::circuit_breaker()` if we add a virtual accessor, or we can add a
direct lookup on the registry.

Current `try_deliver_local` signature:
```cpp
mailbox::EnqueueResult try_deliver_local(ActorId target, TypedMessage msg,
    uint8_t priority = 0, int64_t deadline_ns = 0,
    mailbox::DeliveryOptions options = {});
```

The function already has access to `this` (ActorSystem), which has
`registry_` for actor lookups. Add:

```cpp
// After the get_mailbox() call, before the duplicate check:
if (auto* actor = registry_->find(target)) {
    if (auto* eba = actor->is_event_based_actor()
                        ? static_cast<EventBasedActor*>(actor)
                        : nullptr) {
        if (eba->quarantine_enabled()) {
            auto* cb = eba->circuit_breaker();
            const auto& policy = eba->quarantine_policy();
            auto now = std::chrono::steady_clock::now();

            if (cb->state == CircuitBreakerState::kOpen) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - cb->opened_at);
                if (elapsed >= policy.cooldown_period) {
                    cb->state = CircuitBreakerState::kHalfOpen;
                    cb->half_open_probe_in_flight = true;
                    // Fall through to admit the probe message
                } else {
                    return mailbox::EnqueueResult{
                        mailbox::EnqueueResultCode::CircuitOpen, target};
                }
            }

            if (cb->state == CircuitBreakerState::kHalfOpen) {
                if (cb->half_open_probe_in_flight) {
                    return mailbox::EnqueueResult{
                        mailbox::EnqueueResultCode::CircuitOpen, target};
                }
                // No probe in flight — this message becomes the probe.
                // BUT: we are on a sender thread, not the actor thread.
                // Setting half_open_probe_in_flight here races with
                // record_circuit_breaker_result() on the scheduler thread.
                //
                // Resolution: the probe flag is best-effort. If we race
                // and two probes get through, both will be processed and
                // both will call record_circuit_breaker_result(). The
                // second will see state == kClosed (first probe succeeded)
                // or record another failure. This is safe.
                cb->half_open_probe_in_flight = true;
            }
        }
    }
}
```

**Thread safety of probe flag:** As noted in the comment, setting
`half_open_probe_in_flight` from a non-owner thread is a benign race. The outcome
is that occasionally two probe messages pass through instead of one. Both get
processed normally, and the circuit breaker converges correctly — either to Closed
(both succeed, first one already closed it) or back to Open (any failure re-opens).
This is an acceptable tradeoff that avoids locking on the hot path.

### 3.2 G5: Timeout Rate Integration

**File:** `src/actor/event_based_actor.cpp`, `record_circuit_breaker_result()`

Currently the function only checks `failure_rate_threshold` via the failure EMA.
Timeout rate tracking already exists in `FailureRateTracker` but is never wired to
the circuit breaker.

**Design:** Add a `record_circuit_breaker_timeout()` method on `EventBasedActor`,
or extend `record_circuit_breaker_result()` with an overload/mode parameter.

```cpp
void EventBasedActor::record_circuit_breaker_timeout() {
    if (!quarantine_policy_.enabled) return;
    if (quarantine_policy_.timeout_rate_threshold == 0) return;

    auto now = std::chrono::steady_clock::now();
    failure_rate_tracker_.advance_buckets(now);
    failure_rate_tracker_.record_timeout();
    auto window_ms = static_cast<uint32_t>(
        quarantine_policy_.observation_window.count());

    double timeout_rate = failure_rate_tracker_.timeout_rate(window_ms);

    if (timeout_rate > static_cast<double>(
            quarantine_policy_.timeout_rate_threshold)) {
        trip_circuit(now); // shared trip logic
    }
}
```

The timeout recording is called from the ask/request timeout path — when
`context()->ask()` or `context()->request()` times out waiting for a response, the
caller's actor records a timeout on the target actor's circuit breaker tracker.

**Integration point:** `ActorContext::handle_ask_timeout()` or equivalent.

This is P1 — it requires identifying all timeout sources and plumbing them to the
target actor's tracker. For the initial P0 implementation, failure-rate tripping
alone provides substantial protection.

### 3.3 G6: Mailbox Pressure Integration

**Deferred to P2.** The `mailbox_pressure_threshold` field in `QuarantinePolicy`
requires periodic polling of the mailbox pressure ratio and comparison against the
threshold. This can be done in the scheduler's actor processing loop (check pressure
before each message dispatch) or via a periodic timer. The design is straightforward
but adds complexity — deferring until the core circuit breaker is validated.

### 3.4 G7: CLI `/actor <id> circuit`

**Files:** `src/actor/event_based_actor.cpp` (InspectStateReply), `src/cli/` (command
registration)

**Design:** Extend the existing `InspectStateRequest` / `InspectStateReply` protocol
with circuit breaker fields. The CLI parses `/actor <id> circuit` as syntactic sugar
for `/actor <id> inspect --circuit`.

**Proto changes (in `cli_messages.proto`):**

```protobuf
message InspectStateRequest {
    // ... existing fields ...
    bool include_circuit_breaker = 10;  // NEW
    bool include_quarantine_info = 11;  // NEW
}

message InspectStateReply {
    // ... existing fields ...
    // NEW fields:
    string circuit_state = 20;         // "closed", "open", "half_open"
    uint32 circuit_trip_count = 21;
    double circuit_failure_ema = 22;
    uint64 circuit_opened_at_ns = 23;  // 0 if not currently open
    bool quarantine_enabled = 24;
    string quarantine_reason = 25;     // empty if not quarantined
}
```

**EventBasedActor::receive()** already handles `InspectStateRequestTag` (line 204).
Add the new fields to the reply:

```cpp
if (req.include_circuit_breaker()) {
    if (quarantine_policy_.enabled) {
        auto* pb_cb = reply.mutable_circuit_breaker();
        pb_cb->set_state(to_string(circuit_breaker_.state));
        pb_cb->set_trip_count(circuit_breaker_.trip_count);
        pb_cb->set_failure_ema(circuit_breaker_.failure_ema);
        if (circuit_breaker_.state == CircuitBreakerState::kOpen) {
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                circuit_breaker_.opened_at.time_since_epoch()).count();
            pb_cb->set_opened_at_ns(static_cast<uint64_t>(ns));
        }
    }
}
if (req.include_quarantine_info()) {
    if (auto* lc = as_lifecycle()) {
        if (lc->is_quarantined()) {
            reply.set_quarantine_reason(to_string(lc->quarantine_reason()));
        }
    }
    reply.set_quarantine_enabled(quarantine_policy_.enabled);
}
```

**CLI command registration** is done in `CliActor` or a command registration file.
The pattern is adding a new `CommandNode` to the `/actor` subtree.

### 3.5 G8: CLI Quarantine/Unquarantine Commands

**Deferred to P2.** These require:
- `/actor <id> quarantine [--reason <text>]` — sets `LifecycleState::kQuarantined`
- `/actor <id> unquarantine` — transitions Quarantined→Stopped→Starting→Active

The unquarantine path re-initializes the actor through its supervisor (same restart
path as after a stop). The supervisor must be identified and the restart triggered.

### 3.6 G9: Supervision Escalation to Quarantine

**File:** `src/supervision/supervision.cpp` (or `SelfSupervisingActor::decide_restart`)

**Design:** When a child exceeds `max_restarts` within the restart window AND
`quarantine_policy_.escalate_on_max_restarts` is true, the supervisor issues
`SupervisionDirective::Quarantine` instead of `SupervisionDirective::Stop`.

The `SupervisionDirective::Quarantine` value already exists in the enum. The
supervisor's decision logic in `SelfSupervisingActor::decide_restart()` needs to
check the policy:

```cpp
SupervisionDirective SelfSupervisingActor::decide_restart(ActorId child_id,
                                                          const error& err) {
    auto& counts = restart_counts_[child_id];
    counts++;
    auto now = std::chrono::steady_clock::now();

    // Check if we're in a new restart window
    if (first_failure_time_ == std::chrono::steady_clock::time_point{}) {
        first_failure_time_ = now;
    } else if (now - first_failure_time_ > policy_.restart_interval) {
        // New window — reset counts
        first_failure_time_ = now;
        for (auto& [id, count] : restart_counts_) count = 0;
        counts = 1;
    }

    if (counts > policy_.max_restarts) {
        // NEW: check quarantine escalation
        if (policy_.quarantine.enabled &&
            policy_.quarantine.escalate_on_max_restarts) {
            return SupervisionDirective::Quarantine;
        }
        return SupervisionDirective::Stop;
    }
    return SupervisionDirective::Restart;
}
```

The transition to `kQuarantined` in the child actor is triggered by the supervisor
sending a lifecycle transition message (or calling `transition_to_quarantined()`
directly if the supervisor has a reference to the child).

### 3.7 G10: Circuit State Metric

**File:** `src/actor/event_based_actor.cpp`, `record_circuit_breaker_result()`

Add a metric emission when the circuit breaker changes state:

```cpp
// In record_circuit_breaker_result(), after state transition:
if (metrics_ring_buffer_) [[unlikely]] {
    metrics::MetricEvent evt{};
    evt.actor_id = id();
    evt.event_type = metrics::MetricEventType::kCircuitStateChange;
    evt.code = static_cast<uint8_t>(circuit_breaker_.state);
    evt.value_hi = circuit_breaker_.trip_count;
    evt.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    metrics_ring_buffer_->try_push(evt);
}
```

`MetricEventType::kCircuitStateChange` already exists (value 23 per the design doc).

## 4. Data Flow: Full Circuit Breaker Lifecycle

### 4.1 Closed → Open (Trip)

```
Sender: try_deliver_local(target, msg)
  → circuit breaker state == kClosed → admit message
  → mailbox->try_push(msg) → Accepted
Actor scheduler: receive(msg)
  → deserialize/handler fails
  → record_circuit_breaker_result(false)
    → advance_buckets(now)
    → record_failure()
    → failure_ema = α * failure_rate + (1-α) * failure_ema
    → if failure_ema > threshold:
      → state = kOpen
      → opened_at = now
      → trip_count++
      → emit metric: kCircuitStateChange{state=open, trips}

Sender: try_deliver_local(target, next_msg)
  → circuit breaker state == kOpen
  → elapsed = now - opened_at
  → if elapsed < cooldown_period:
    → return EnqueueResult{CircuitOpen}
  → if elapsed >= cooldown_period:
    → state = kHalfOpen, half_open_probe_in_flight = true
    → admit message (probe)
```

### 4.2 HalfOpen → Closed (Success)

```
Actor scheduler: receive(probe_msg)
  → handler succeeds
  → record_circuit_breaker_result(true)
    → state == kHalfOpen
    → state = kClosed
    → trip_count = 0
    → half_open_probe_in_flight = false
    → emit metric: kCircuitStateChange{state=closed, trips=0}

Sender: try_deliver_local(target, next_msg)
  → circuit breaker state == kClosed → admit normally
```

### 4.3 HalfOpen → Open (Probe Failure)

```
Actor scheduler: receive(probe_msg)
  → handler fails
  → record_circuit_breaker_result(false)
    → state == kHalfOpen
    → state = kOpen
    → opened_at = now
    → half_open_probe_in_flight = false
    → trip_count++
    → if trip_count >= max_circuit_trips:
      → transition_to_quarantined(CircuitBreakerTrip)
    → emit metric: kCircuitStateChange{state=open, trips=N}
```

### 4.4 Quarantined Actor Message Rejection

```
Sender: try_deliver_local(target, msg)
  → actor lifecycle state == kQuarantined
  → build FailureEnvelope(Quarantined, ...)
  → return EnqueueResult{...} or equivalent rejection
  → emit metric: kDeliveryFailure{reason=quarantined}
```

Note: The quarantine check in `try_deliver_local()` is separate from the circuit
breaker check. An actor can be quarantined without a circuit breaker, and an actor
can have a circuit breaker trip without quarantine escalation.

## 5. New and Modified Files

| File | Action | Purpose |
|------|--------|---------|
| `include/hpactor/mailbox/mailbox_policy.hpp` | Modify | Add `CircuitOpen = 10` to `EnqueueResultCode` |
| `src/actor/actor_system.cpp` | Modify | Add pre-admission circuit breaker gate (G1–G4) |
| `src/actor/event_based_actor.cpp` | Modify | Add `record_circuit_breaker_timeout()` (G5); emit circuit state metric (G10) |
| `src/supervision/supervision.cpp` | Modify | Wire quarantine escalation in `decide_restart()` (G9) |
| `src/cli/cli_messages.proto` | Modify | Add circuit/quarantine fields to InspectStateRequest/Reply (G7) |
| `src/cli/` (command registration) | Modify | Add `/actor <id> circuit` CLI command (G7) |
| `tests/unit/core/test_circuit_breaker_delivery.cpp` | **New** | Unit tests for pre-admission gating |
| `tests/unit/actor/test_circuit_breaker_result.cpp` | **New** | Unit tests for record_circuit_breaker_result state machine |
| `tests/unit/supervision/test_supervision_quarantine.cpp` | **New** | Unit tests for supervision→quarantine escalation |
| `tests/integration/test_circuit_breaker_lifecycle.cpp` | **New** | Integration test: full Closed→Open→HalfOpen→Closed cycle |

## 6. Test Plan

### 6.1 Phase 1: Pre-Admission Gate (RED→GREEN)

**Test: `CircuitBreakerDeliveryTest.OpenCircuitRejectsMessage`**
- Setup: Actor with `QuarantinePolicy{enabled=true}`, circuit manually set to `kOpen`
- Action: `try_deliver_local(target, msg)`
- Assert: Returns `EnqueueResultCode::CircuitOpen`

**Test: `CircuitBreakerDeliveryTest.ClosedCircuitAdmitsMessage`**
- Setup: Actor with quarantine enabled, circuit is `kClosed` (default)
- Action: `try_deliver_local(target, msg)`
- Assert: Returns `EnqueueResultCode::Accepted`

**Test: `CircuitBreakerDeliveryTest.HalfOpenWithProbeInFlightRejects`**
- Setup: Circuit is `kHalfOpen`, `half_open_probe_in_flight = true`
- Action: `try_deliver_local(target, msg)`
- Assert: Returns `EnqueueResultCode::CircuitOpen`

**Test: `CircuitBreakerDeliveryTest.HalfOpenWithoutProbeAdmitsOne`**
- Setup: Circuit is `kHalfOpen`, `half_open_probe_in_flight = false`
- Action: `try_deliver_local(target, msg)`
- Assert: Returns `EnqueueResultCode::Accepted`

**Test: `CircuitBreakerDeliveryTest.CooldownExpiryTransitionsOpenToHalfOpen`**
- Setup: Circuit `kOpen`, `opened_at = now - cooldown_period - 1ms`
- Action: `try_deliver_local(target, msg)`
- Assert: `circuit_breaker().state == kHalfOpen`, message admitted

**Test: `CircuitBreakerDeliveryTest.CooldownNotExpiredKeepsOpen`**
- Setup: Circuit `kOpen`, `opened_at = now - cooldown_period + 100ms`
- Action: `try_deliver_local(target, msg)`
- Assert: `circuit_breaker().state == kOpen`, returns `CircuitOpen`

**Test: `CircuitBreakerDeliveryTest.NoQuarantineNoOverhead`**
- Setup: Actor WITHOUT quarantine enabled
- Action: `try_deliver_local(target, msg)`
- Assert: Returns `Accepted` (no circuit breaker check, no overhead)

### 6.2 Phase 2: Result Recording State Machine

**Test: `CircuitBreakerResultTest.HalfOpenSuccessTransitionsToClosed`**
- Setup: Circuit `kHalfOpen`, `trip_count = 2`
- Action: `record_circuit_breaker_result(true)`
- Assert: `state == kClosed`, `trip_count == 0`, `half_open_probe_in_flight == false`

**Test: `CircuitBreakerResultTest.HalfOpenFailureTransitionsBackToOpen`**
- Setup: Circuit `kHalfOpen`, `trip_count = 1`, `opened_at` set
- Action: Inject failures to exceed threshold, then `record_circuit_breaker_result(false)`
- Assert: `state == kOpen`, `trip_count == 2`

**Test: `CircuitBreakerResultTest.RepeatedTripsEscalateToQuarantine`**
- Setup: `max_circuit_trips = 2`, circuit just tripped to Open (trip_count = 2)
- Action: `record_circuit_breaker_result(false)` (triggers trip which increments to 3)
- Assert: Actor lifecycle state == `kQuarantined`, quarantine_reason == `CircuitBreakerTrip`

**Test: `CircuitBreakerResultTest.ClosedIgnoresSuccess`**
- Setup: Circuit `kClosed`
- Action: `record_circuit_breaker_result(true)`
- Assert: `state == kClosed`, no change

**Test: `CircuitBreakerResultTest.ClosedTripsOnEmaExceedingThreshold`**
- Setup: `failure_rate_threshold = 10`, pre-populate failure buckets to produce rate > 10/sec
- Action: `record_circuit_breaker_result(false)` (pushes EMA over threshold)
- Assert: `state == kOpen`, `trip_count == 1`

**Test: `CircuitBreakerResultTest.NoTripWhenThresholdIsZero`**
- Setup: `failure_rate_threshold = 0` (disabled)
- Action: `record_circuit_breaker_result(false)`
- Assert: `state == kClosed` (threshold 0 means no tripping)

### 6.3 Phase 3: Supervision → Quarantine Escalation

**Test: `SupervisionQuarantineTest.MaxRestartsExceededWithEscalationEnablesQuarantine`**
- Setup: `SupervisionPolicy{max_restarts=2, quarantine={enabled=true, escalate_on_max_restarts=true}}`
- Action: Child fails 3 times within the restart window
- Assert: `SupervisionDirective::Quarantine` is returned

**Test: `SupervisionQuarantineTest.MaxRestartsExceededWithoutEscalationStops`**
- Setup: `quarantine.escalate_on_max_restarts = false`
- Action: Child fails 3 times within the window
- Assert: `SupervisionDirective::Stop` (existing behavior preserved)

**Test: `SupervisionQuarantineTest.QuarantineNotEnabledDoesNotEscalate`**
- Setup: `quarantine.enabled = false`
- Action: Child fails 3 times within the window
- Assert: `SupervisionDirective::Stop`

### 6.4 Phase 4: Integration Tests

**Test: `CircuitBreakerIntegrationTest.FullTripToQuarantineLifecycle`**
- Setup: Actor system with 1 scheduler thread, actor with circuit breaker enabled
  (`failure_rate_threshold=5`, `max_circuit_trips=2`, `observation_window_ms=5000`)
- Action: Send 20 messages, all of which fail in handler → circuit trips
  → cooldown → probe fails (circuit re-opens) → trips again → quarantine escalated
- Assert:
  1. Circuit state goes: Closed → Open → HalfOpen (cooldown) → Open → ... → Quarantined
  2. Messages sent while circuit is Open receive `CircuitOpen` failure
  3. Quarantined actor rejects subsequent messages with `Quarantined`
  4. System messages (InspectState) still work on quarantined actor

**Test: `CircuitBreakerIntegrationTest.ProbeSuccessClosesCircuit`**
- Setup: Same as above
- Action: Trip circuit → cooldown → probe message succeeds → circuit closes
- Assert:
  1. Circuit state: Closed → Open → HalfOpen → Closed
  2. Subsequent messages flow normally
  3. `trip_count` reset to 0

**Test: `CircuitBreakerIntegrationTest.HealthyActorNeverTrips`**
- Setup: Actor with circuit breaker, `failure_rate_threshold=5`
- Action: Send 100 messages, all succeed
- Assert: Circuit never leaves Closed state

## 7. Implementation Phases

### Phase 0 (Pre-work): Verify Existing Tests Pass

```bash
ctest -R "CircuitBreaker|FailureRate|QuarantineReason" --output-on-failure
```

Confirm all existing unit tests are green before making changes.

### Phase 1: Pre-Admission Gate + EnqueueResultCode (G1–G4)

1. Add `CircuitOpen = 10` to `EnqueueResultCode` in `mailbox_policy.hpp`
2. Add circuit breaker admission check in `try_deliver_local()` in `actor_system.cpp`
3. Write and pass tests from Section 6.1

**RED:** `test_circuit_breaker_delivery.cpp` — all 7 tests fail (gate not implemented)
**GREEN:** Implement gate → all 7 tests pass
**REFACTOR:** Extract circuit breaker check into a helper function

### Phase 2: Result Recording Verification (Existing Code Validation)

1. The existing `record_circuit_breaker_result()` implementation is sound
2. Write the tests from Section 6.2 — these should PASS on current code
3. If any test fails, fix the implementation
4. The goal is to formalize the existing behavior with tests

### Phase 3: Circuit State Metric (G10)

1. Add `kCircuitStateChange` metric emission in `record_circuit_breaker_result()`
2. Verify metric appears in ring buffer on state transitions

### Phase 4: Supervision Escalation (G9)

1. Wire `quarantine.escalate_on_max_restarts` in supervision decide_restart logic
2. Write and pass tests from Section 6.3

### Phase 5: CLI Circuit Inspection (G7)

1. Add proto fields to `InspectStateRequest` / `InspectStateReply`
2. Add circuit breaker fields to `EventBasedActor::receive()` InspectState handler
3. Register `/actor <id> circuit` CLI command
4. Test: CLI output shows correct circuit state

### Phase 6: Integration Tests (G11)

1. Write and pass tests from Section 6.4
2. These require the full actor system (scheduler, mailbox, actor lifecycle)

### Phase 7 (P1): Timeout Rate Integration (G5)

1. Add `record_circuit_breaker_timeout()` method
2. Wire into ask/request timeout paths
3. Write timeout-specific tests

### Phase 8 (P2): CLI Quarantine/Unquarantine (G8)

1. Implement `/actor <id> quarantine` and `/actor <id> unquarantine` commands
2. Write CLI integration tests

### Phase 9 (P2): Mailbox Pressure Integration (G6)

1. Add periodic pressure check in scheduler dispatch loop
2. Wire to circuit breaker trip logic
3. Write pressure-specific tests

## 8. Acceptance Criteria

1. **Circuit breaker trips on failure rate:** When failure rate EMA exceeds
   `failure_rate_threshold` within `observation_window`, circuit transitions to Open.
2. **Open circuit rejects messages at admission:** `try_deliver_local()` returns
   `EnqueueResultCode::CircuitOpen` when circuit is `kOpen`.
3. **Cooldown expiry transitions to HalfOpen:** After `cooldown_period` elapses,
   the next `try_deliver_local()` call transitions Open→HalfOpen and admits exactly
   one probe message.
4. **HalfOpen probe success closes circuit:** Successful processing of the probe
   message transitions HalfOpen→Closed, resets `trip_count` to 0.
5. **HalfOpen probe failure re-opens circuit:** Failed probe transitions
   HalfOpen→Open, increments `trip_count`, restarts cooldown.
6. **Repeated trips escalate to quarantine:** When `trip_count >= max_circuit_trips`,
   actor transitions to `kQuarantined` with `QuarantineReason::CircuitBreakerTrip`.
7. **Quarantined actor rejects user messages:** All user messages to a quarantined
   actor return `FailureEnvelope(Quarantined)`. System messages (InspectState, Stop,
   Kill, supervision) still work.
8. **Supervision escalation to quarantine:** When `escalate_on_max_restarts` is set
   and a child exceeds `max_restarts`, supervisor issues `Quarantine` directive
   instead of `Stop`.
9. **Zero overhead when disabled:** Actors without `quarantine.enabled = true` incur
   no circuit breaker checks, no extra allocations, no metric emissions.
10. **Existing tests continue to pass:** No regressions in existing unit,
    integration, or system tests.
11. **CLI `/actor <id> circuit`** shows circuit breaker state, trip count, and EMA.
12. **Circuit state metric** is emitted on every state transition.

## 9. References

- [ACT-005 Issue](https://github.com/skg7on/HPActor/issues/10)
- [Actor Quarantine & Circuit Breaker Architecture Design](../architecture/production/actor-quarantine-circuit-breaker-design.md)
- [Structured Failure Envelope Design](../architecture/production/structured-failure-envelope-design.md)
- [Cluster Failure Model Design](../architecture/production/cluster-failure-model-design.md)
- [Architecture Requirement Backlog](../architecture/production/architecture-requirement-backlog.md)
