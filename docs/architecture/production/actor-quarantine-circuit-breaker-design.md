# Actor Quarantine & Circuit Breaker Architecture Design

## 1. Executive Summary

HPActor's supervision tree already tracks per-child restart counts and enforces
max-restart windows. However, an actor that repeatedly fails at a rate just
below the supervision threshold, overloads its mailbox, or times out on every
request can continue receiving traffic and destabilize callers indefinitely.

This design adds two complementary runtime protections:

- **Quarantine**: terminal isolation for an actor that supervision declares
  unrecoverable. Quarantined actors reject all user messages (returning
  `FailureReason::Quarantined`) while still accepting system messages for
  inspection, stop, and operator-driven recovery.
- **Circuit Breaker**: a self-resetting failure-rate guard that trips open when
  failures, timeouts, or mailbox pressure exceed configured thresholds, then
  auto-transitions through half-open probing back to closed.

These work at the actor level and are opt-in via TOML policy, with safe defaults
that preserve existing behavior for actors that don't configure them.

## 2. Current State Audit

### 2.1 Already Implemented

| Component | Location | What It Provides |
|---|---|---|
| `FailureReason::Quarantined = 12` | `types/failure_reason.hpp:39` | Canonical reason code already defined |
| `FailureReason::CircuitOpen = 13` | `types/failure_reason.hpp:40` | Canonical reason code already defined |
| `FailureSource::Supervision` | `types/failure_reason.hpp:97` | Source classification for quarantine events |
| `FailureEnvelope` | `types/failure_envelope.hpp` | Structured failure carrier -- already maps `Quarantined` |
| `retryable(FailureReason)` | `types/failure_reason.hpp:107` | `Quarantined` is non-retryable (intentional), `CircuitOpen` is retryable (transient) |
| `LifecycleState` (7 states) | `actor/lifecycle_state.hpp:10` | Starting/Active/Draining/Stopping/Stopped/Failed/Recovering -- `kQuarantined` still missing |
| `SupervisionPolicy` | `supervision/supervision.hpp:41` | `max_restarts`, `restart_interval`, strategy -- no quarantine escalation yet |
| `SupervisorActor::restart_counts_` | `supervision/supervision.hpp:79` | Per-child restart tracking with sliding window |
| `SelfSupervisingActor` | `supervision/supervision.hpp:88` | Self-managed children with restart counts -- no quarantine escalation |
| `EventBasedActor::receive()` | `actor/event_based_actor.hpp` | TypedMessage dispatch -- gates system messages from CLI path |
| `LifecycleActor` mixin | `actor/lifecycle_actor.hpp` | Opt-in lifecycle state machine |
| `DrainPolicy` / `DrainConfig` | `actor/drain_config.hpp` | Per-actor drain configuration |
| `MetricEventType::kDeliveryFailure = 20` | `metrics/metrics_event.hpp` | Delivery failure metric event |
| Actor metrics ring buffer | `metrics/` subsystem | MpscRingBuffer instrumentation |
| CLI `/actor` command tree | `cli/` subsystem | Trie-based commands, InspectState protocol |
| `try_deliver_local()` | `actor_system.cpp` | Builds FailureEnvelope on delivery failures |

### 2.2 What's Missing

- No `kQuarantined` state in `LifecycleState` or transition definition
- No `QuarantinePolicy` or `CircuitBreakerPolicy` config
- No failure-rate/timeout-rate tracking per actor
- No circuit breaker state machine (Closed -> Open -> HalfOpen -> Closed)
- Supervision max-restarts triggers stop -- no quarantine escalation option
- No quarantine-aware message gating in `EventBasedActor::receive()`
- No operator override CLI commands
- No `hpactor_actor_quarantine_total` or `hpactor_actor_circuit_state` metrics
- No quarantine audit logging

## 3. Design

### 3.1 Quarantine State Machine

Quarantine is a terminal isolation state within the lifecycle. Once quarantined,
an actor does NOT self-recover -- recovery requires explicit operator action or
supervisor-level restart.

```
                    +----------+
    supervision     |          |  operator override
    max restarts -->| QUARANT- | (CLI / admin API)
    exceeded        |  INED    |--> kStopped -> kStarting -> kActive
                    |          |
                    +----------+
                         |
                         +-- Accepts: InspectState, Stop, Kill, supervision msgs
                         +-- Rejects: all user messages (-> FailureEnvelope w/ Quarantined)
                         +-- Rejects: spawn requests, RPC dispatch
```

**Quarantine triggers** (any or combination):

| Trigger | Source | Mechanism |
|---|---|---|
| Repeated failure | Supervision | `max_restarts` exceeded in window -> escalate to quarantine instead of stop |
| Timeout flood | Circuit breaker | Timeout rate exceeds threshold -> circuit opens, quarantine on repeated open |
| Mailbox overload | Circuit breaker | Mailbox pressure above threshold for sustained period -> quarantine |
| Manual | Operator | CLI `/actor <id> quarantine` for unsafe/troubleshooting isolation |

### 3.2 LifecycleState Extension

Add `kQuarantined = 7` to the existing enum and update the state transition table:

```cpp
enum class LifecycleState : uint8_t {
    kStarting = 0,
    kActive = 1,
    kDraining = 2,
    kStopping = 3,
    kStopped = 4,
    kFailed = 5,
    kRecovering = 6,
    kQuarantined = 7,  // NEW
};
```

New `StateDef` entry for `kQuarantined`:

```cpp
{LifecycleState::kQuarantined, "quarantined",
 false /* no user msgs */, true /* system msgs ok */,
 1, {LifecycleState::kStopped}},  // only Stopped is legal next state
```

Transitions **into** `kQuarantined` (add to existing state transition lists):
- `kActive` -> `kQuarantined`: supervision escalation or circuit breaker trip
- `kFailed` -> `kQuarantined`: `max_restarts` exceeded with quarantine policy
- `kRecovering` -> `kQuarantined`: recovery failure with quarantine policy

### 3.3 QuarantineReason

Why was the actor quarantined? Distinct from `FailureReason` -- this is the
trigger classification, not the delivery outcome:

```cpp
enum class QuarantineReason : uint8_t {
    SupervisionEscalation = 0,  ///< max_restarts exceeded in window
    CircuitBreakerTrip = 1,     ///< failure/timeout rate threshold exceeded
    MailboxPressure = 2,        ///< sustained mailbox overload
    OperatorAction = 3,         ///< explicit CLI/admin quarantine
    RecoveryFailure = 4,        ///< recovery attempt failed
};
```

### 3.4 QuarantinePolicy

Per-actor TOML-configurable policy:

```cpp
struct QuarantinePolicy {
    bool enabled = false;  // default off -- preserves existing behavior
    bool escalate_on_max_restarts = true;  // quarantine instead of stop
    uint32_t failure_rate_threshold = 0;   // failures/sec, 0 = disabled
    uint32_t timeout_rate_threshold = 0;   // timeouts/sec, 0 = disabled
    float mailbox_pressure_threshold = 0.0f;  // ratio 0.0-1.0, 0 = disabled
    std::chrono::milliseconds cooldown_period{30'000};  // 30s cooldown
    std::chrono::milliseconds observation_window{10'000};  // 10s sliding window
};
```

### 3.5 Circuit Breaker State Machine

The circuit breaker is a per-actor state machine that operates independently
of quarantine but can escalate to quarantine on repeated trips:

```
     +----------+
     |          |
     |  CLOSED  |  (normal operation -- messages flow through)
     |          |
     +-----+----+
           | failure/timeout/pressure
           | threshold exceeded
           v
     +----------+
     |          |
     |   OPEN   |  (all user messages rejected w/ CircuitOpen)
     |          |
     +-----+----+
           | cooldown expires
           v
     +----------+
     |          |
     | HALF_OPEN|  (single probe message allowed through)
     |          |
     +-----+----+
           |
      +----+----+
      |         |
   success   failure
      |         |
      v         v
   CLOSED    OPEN (re-arm cooldown)

   Repeated OPEN -> QUARANTINE (if quarantine escalation configured)
```

```cpp
enum class CircuitBreakerState : uint8_t {
    kClosed = 0,
    kOpen = 1,
    kHalfOpen = 2,
};

struct CircuitBreakerTracker {
    CircuitBreakerState state{CircuitBreakerState::kClosed};
    uint32_t trip_count{0};
    uint32_t half_open_probe_count{0};
    Clock::time_point opened_at;
    Clock::time_point half_open_at;
    double failure_ema{0.0};
    double timeout_ema{0.0};
    Clock::time_point last_evaluation;
};
```

### 3.6 FailureTracker -- Sliding Window Rate Monitor

Per-actor sliding window counters for failure and timeout rate calculation.
Uses a ring buffer of time-bucketed counts for O(1) window summation.

```cpp
struct FailureRateTracker {
    static constexpr size_t kNumBuckets = 10;
    std::array<uint32_t, kNumBuckets> failure_buckets{};
    std::array<uint32_t, kNumBuckets> timeout_buckets{};
    uint32_t total_failures{0};
    uint32_t total_timeouts{0};
    size_t current_bucket{0};
    size_t bucket_interval_ms;  // observation_window / kNumBuckets
    Clock::time_point last_bucket_advance;

    void record_failure();
    void record_timeout();
    void advance_buckets(Clock::time_point now);
    double failure_rate() const;   // failures/sec over window
    double timeout_rate() const;   // timeouts/sec over window
};
```

### 3.7 Integration Points

#### 3.7.1 Supervision Integration

Supervision already tracks `restart_counts_` per child with a sliding window
(`first_failure_time_`). When `max_restarts` is exceeded:

**Before (current):** `SupervisionDirective::Stop` -- actor is stopped.
**After (with quarantine):** If `QuarantinePolicy::escalate_on_max_restarts`,
the supervisor transitions the child to `kQuarantined` instead of stopping it.

The `SupervisionDirective` enum gains a `Quarantine` value.

#### 3.7.2 Message Delivery Integration

`EventBasedActor::receive()` already intercepts system messages (LinkMsg,
UnlinkMsg, DownMsg, CLI TypeTags). The quarantine gate is inserted before user
message dispatch:

```
receive(msg):
    if lifecycle_state == kQuarantined:
        if msg is system message (InspectState, Stop, Kill, supervision):
            -> dispatch normally
        else:
            -> build FailureEnvelope(Quarantined, quarantine_reason_detail)
            -> send back to sender (if sender info available)
            -> record metric: kActorQuarantineRejected
            -> log warning: "quarantined actor <id> rejected message type=<t>"
            -> return (do not dispatch to behavior)
    // existing dispatch flow...
```

`try_deliver_local()` in `src/actor/actor_system.cpp` already builds
`FailureEnvelope` on `ActorNotFound` and mailbox rejection. It gains a
quarantine check: if the target actor is quarantined, build
`FailureEnvelope(Quarantined)` and return early.

#### 3.7.3 Circuit Breaker in Delivery Path

Before `try_deliver_local()` admits a message to the mailbox, if the actor
has a `CircuitBreakerTracker` in `kOpen` state:
- Build `FailureEnvelope(CircuitOpen)`, return to sender
- If in `kHalfOpen` state: allow ONE message through, track outcome

The circuit breaker evaluation happens at message admission time (in the
scheduler dispatch, before mailbox injection).

#### 3.7.4 Lifecycle State Integration

`LifecycleActor` (the opt-in mixin) gains quarantine transition methods:

```cpp
class LifecycleActor {
    // ...
    result<void> transition_to_quarantined(QuarantineReason reason);
    result<void> transition_from_quarantined();  // operator override
    bool is_quarantined() const;
    QuarantineReason quarantine_reason() const;
};
```

### 3.8 Data Flow: Supervision Escalation to Quarantine

```
SupervisorActor::handle_child_down()
  -> increment restart_counts_[child_id]
  -> check max_restarts in window
  -> IF exceeded:
      IF quarantine_policy_.escalate_on_max_restarts:
          -> child->transition_to_quarantined(SupervisionEscalation)
          -> log structured: "actor <id> quarantined: supervision escalation"
          -> emit metric: hpactor_actor_quarantine_total{reason="supervision_escalation"}
      ELSE:
          -> SupervisionDirective::Stop (existing behavior)
  -> IF NOT exceeded:
      -> SupervisionDirective::Restart (existing behavior)
```

### 3.9 Data Flow: Circuit Breaker Trip

```
try_deliver_local(msg, target)
  -> lookup target actor
  -> IF target has circuit_breaker && circuit_breaker.state == kOpen:
      -> if cooldown elapsed: transition to kHalfOpen, allow probe
      -> else: build FailureEnvelope(CircuitOpen), return
  -> IF target has circuit_breaker && circuit_breaker.state == kHalfOpen:
      -> if probe_count > 0: build FailureEnvelope(CircuitOpen), return
      -> else: allow through (probe_count = 1)

on_message_processing_failure(actor, reason):
  -> IF reason is timeout or processing error:
      -> circuit_breaker.failure_ema = update_ema(...)
      -> IF failure_ema > threshold:
          -> IF trip_count >= max_trips_before_quarantine:
              -> actor->transition_to_quarantined(CircuitBreakerTrip)
          -> ELSE:
              -> circuit_breaker.state = kOpen
              -> circuit_breaker.opened_at = now
              -> emit metric: hpactor_actor_circuit_state{state="open"}

on_message_processing_success(actor):
  -> IF circuit_breaker.state == kHalfOpen:
      -> circuit_breaker.state = kClosed
      -> circuit_breaker.trip_count = 0
      -> emit metric: hpactor_actor_circuit_state{state="closed"}
```

## 4. Configuration

### 4.1 TOML Schema

```toml
# Per-actor quarantine policy (in actor definition)
[[actors]]
name = "payment-processor"
behavior = "payment_processor"
# ...
[actors.quarantine]
enabled = true
escalate_on_max_restarts = true
failure_rate_threshold = 10      # 10 failures/sec in observation window
timeout_rate_threshold = 5       # 5 timeouts/sec
mailbox_pressure_threshold = 0.9 # 90% capacity sustained
cooldown_period_ms = 30000       # 30s circuit breaker cooldown
observation_window_ms = 10000    # 10s sliding window
max_circuit_trips = 3            # quarantine after 3rd consecutive trip

# Global defaults (optional, in [system.quarantine])
[system.quarantine]
default_enabled = false
default_cooldown_period_ms = 30000
default_observation_window_ms = 10000
```

### 4.2 Config Parsing

New self-registering subsystem parser in `src/config/parsers/quarantine_parser.cpp`.
Follows the existing IoC pattern: `TomlSystemParserRegistration<QuarantineConfigParser>`.
Parser interface uses opaque `TomlTableView`.

## 5. Observability

### 5.1 Metrics

| Metric | Type | Description |
|---|---|---|
| `hpactor_actor_quarantine_total` | Counter | Incremented on quarantine transitions, with `reason` label |
| `hpactor_actor_unquarantine_total` | Counter | Incremented on operator override unquarantine |
| `hpactor_actor_circuit_state` | Gauge | 0=closed, 1=open, 2=half-open; per-actor label |
| `hpactor_actor_circuit_trips_total` | Counter | Total circuit breaker trip events |

Metric events added to `MetricEventType` enum:
- `kActorQuarantined = 21`
- `kActorUnquarantined = 22`
- `kCircuitStateChange = 23`

### 5.2 Logging

Structured log events at WARN level:
- `actor_quarantined`: `{actor_id, reason, failure_count, window_ms, detail}`
- `actor_unquarantined`: `{actor_id, source ("operator" | "supervisor")}`
- `circuit_state_change`: `{actor_id, old_state, new_state, trip_count, ema_rate}`

### 5.3 CLI

New commands under existing `/actor` tree:

```
/actor <id> quarantine [--reason <text>]
    Manually quarantine an actor.

/actor <id> unquarantine
    Operator override to release from quarantine.
    Transitions: kQuarantined -> kStopped -> kStarting -> kActive
    (re-initializes through supervisor restart)

/actor <id> circuit
    Show circuit breaker state: state, trip count, EMA failure/timeout rates,
    last state change timestamp.

/actor list --quarantined
    Filter actor list to show only quarantined actors.
```

`InspectStateRequest` gains `include_quarantine_info` and
`include_circuit_breaker_info` flags. `InspectStateReply` gains
`quarantine_reason`, `quarantine_since`, `circuit_state` fields.

### 5.4 Audit Trail

Quarantine transitions are auditable via:
- Structured log entry with W3C trace context
- Metric counter with reason label
- CLI history (visible in `/actor <id> show`)

## 6. Failure Contract

### 6.1 What Senders See

| Scenario | FailureReason | retryable |
|---|---|---|
| Actor quarantined (any reason) | `Quarantined` | false |
| Circuit breaker open | `CircuitOpen` | true |
| Circuit breaker half-open, probe rejected | `CircuitOpen` | true |

### 6.2 What System Callers See

System messages (InspectState, Stop, Kill, supervision LinkMsg/DownMsg) are
never gated by quarantine or circuit breaker. This ensures:
- Operators can always inspect quarantined actors
- Supervisors can always stop or restart quarantined children
- CLI `/actor <id> show` works on quarantined actors

### 6.3 Side Effects

- Quarantined actor's mailbox is NOT drained (messages remain for post-mortem
  inspection via CLI `/actor <id> mailbox`).
- Linked/monitored actors receive `DownMsg` when an actor transitions to
  `kQuarantined` (same as `kStopped`/`kFailed`).
- Dead-letter queue can be configured to capture rejected messages for
  quarantined actors (DLQ `Quarantined` reason already exists in the
  `DeadLetterReason` design).

## 7. Performance & Safety

### 7.1 Fast Path

The quarantine check in `try_deliver_local()` is a single atomic load of
`LifecycleState` and a branch -- negligible overhead on the hot path. Circuit
breaker evaluation (rate tracking, EMA updates) is amortized per-bucket
(~1 evaluation per second for 10s/10buckets).

### 7.2 Concurrency

- `CircuitBreakerTracker` is owned by the actor and only mutated from the
  scheduler thread that owns the actor -- no lock needed.
- `FailureRateTracker` bucket writes are single-writer (scheduler thread).
- `QuarantinePolicy` is immutable after actor construction.
- `LifecycleState` transitions use CAS (same as existing states).

### 7.3 Memory

- `CircuitBreakerTracker`: ~56 bytes per actor
- `FailureRateTracker`: ~120 bytes per actor
- Both allocated within the actor's memory region.
- Actors without quarantine policy enabled incur zero additional allocation.

## 8. Non-Goals (Deferred)

- Node-level quarantine (covered by `cluster-failure-model-design.md`).
- Quarantine propagation to remote nodes (requires cluster control plane).
- Automatic quarantine of callers-of-quarantined-actors.
- Persistent quarantine state across actor restart.
- Quarantine-aware load shedding in the scheduler.
- Circuit breaker for RPC channels (separate concern, uses same patterns).

## 9. Acceptance Criteria

1. Actor transitions to `kQuarantined` when supervision `max_restarts` is
   exceeded and `escalate_on_max_restarts = true`.
2. Actor transitions to `kQuarantined` when circuit breaker trips exceed
   `max_circuit_trips`.
3. Quarantined actor rejects user messages with `FailureEnvelope(Quarantined)`.
4. Quarantined actor still accepts InspectState, Stop, and supervision messages.
5. Circuit breaker transitions Closed -> Open when failure/timeout rate exceeds
   threshold within observation window.
6. Circuit breaker transitions Open -> HalfOpen after cooldown.
7. Successful probe in HalfOpen transitions to Closed.
8. Failed probe in HalfOpen transitions back to Open.
9. Operator `/actor <id> unquarantine` releases quarantine.
10. `/actor <id> circuit` shows current circuit breaker state.
11. Metrics `hpactor_actor_quarantine_total` and `hpactor_actor_circuit_state`
    are emitted and queryable.
12. Audit logs are written on quarantine/unquarantine transitions.
13. Actors without `quarantine.enabled = true` have zero overhead.
14. All existing tests continue to pass.
