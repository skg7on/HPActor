# MBX-006: Remote Outbound Queue Limits & Per-Endpoint Pressure State — Design Spec

**Issue**: [#27](https://github.com/skg7on/HPActor/issues/27)
**Subsystem**: Mailbox / Transport
**Priority**: P1
**Backlog status**: Missing (this design)
**Source requirement**: `docs/architecture/production/architecture-requirement-backlog.md`, MBX-006

## 1. Problem

Even when every actor mailbox is bounded (MBX-001) and overflow policies are in
place (MBX-002), outbound transport queues can grow without bound during slow or
disconnected remote peers. `ConnectionPool` today holds a `std::deque<PendingMessage>`
behind a `max_pending` count limit, but:

- there is no byte budget, so large message payloads bypass the count-only guard;
- there is no pressure state machine at the endpoint level, so the system cannot
  distinguish between "lightly queued" and "about to exhaust memory";
- there is no priority lane for control frames (backpressure signals,
  ACK/NACK), so under user-message overload, control traffic starves;
- there is no circuit breaker to stop sending to an endpoint that fails
  repeatedly, so reconnect storms waste CPU and buffer memory;
- there is no reserved headroom for at-least-once messages, so a flood of
  best-effort messages can crowd out messages the sender has already invested
  outbox tracking in.

The refined requirement backlog (MBX-004 in
`feature-gap-refined-requirement-backlog.md`) describes the same gap under
"Remote Outbound Queue Limits."

### Current state

| Area | Current state |
|---|---|
| Queue storage | `std::deque<PendingMessage>` in `ConnectionPool`, count limit via `PoolConfig::max_pending`. |
| Byte budget | None. |
| Pressure state | `PressureStateMachine` exists at the actor-mailbox level (Normal/SoftPressure/HardPressure/Recovering) but not at the endpoint level. |
| Control lane | Not present — all messages share one FIFO deque. |
| Circuit breaker | Not present at the transport level. Actor-level `CircuitBreakerTracker` exists for actor quarantine (AR-004). |
| Reliable headroom | Not present. |
| Config | `PoolConfig` has `max_pending` only; no TOML subsystem parser for transport outbound limits. |
| Metrics/CLI | No per-endpoint queue depth, pressure state, or rejection counters. |

## 2. Goals

1. Add per-endpoint message count **and** byte budget limits to outbound queues.
2. Add a control lane with hard reserve so system frames (backpressure,
   ACK/NACK) are not starved by user-message overload.
3. Reserve configurable headroom for at-least-once messages within the data lane.
4. Drive a per-endpoint `PressureStateMachine` from queue depth ratio, reusing
   the existing 4-state model, with rate-limited advisory push signals to the
   remote endpoint and authoritative pull results via `try_send()`.
5. Add an endpoint circuit breaker that opens on N consecutive connection
   failures, with cooldown and half-open probing.
6. Expose per-endpoint queue depth, pressure state, circuit breaker state, and
   rejection counters through metrics and CLI.
7. Provide TOML configuration under `[system.transport.outbound]` with safe defaults.
8. Map endpoint-level rejections to canonical `FailureReason` values and
   integrate with the dead-letter queue.

## 3. Non-Goals

- Per-endpoint override configuration. Global defaults only in this iteration
  (per-endpoint overrides can follow the mailbox policy per-actor pattern later).
- Full health scoring for endpoints (latency, decode errors, etc.). The circuit
  breaker trips on connection failures only.
- Durable spill for rejected best-effort messages. Rejected messages are DLQ'd
  if configured, dropped otherwise. Reliable messages already have their own
  outbox tracker (MSG-003).
- Changing the wire protocol or frame format. Backpressure signals use the
  existing `TypeTag::BackpressureSignalTag` and `ActorMsgFrame` path from MBX-003.
- Endpoint drain rate estimation for anything beyond `retry_after` hints.
  Admission decisions never use estimated drain rate.

## 4. Design

### 4.1 Architecture Overview

```
ConnectionPool
  └── EndpointOutboundQueue  (replaces bare std::deque<PendingMessage>)
        ├── control_lane (backpressure signals, ACK/NACK, system frames)
        │     └── hard reserve: MIN(max_messages * 10%, 64) slots
        ├── data_lane (user messages)
        │     ├── best_effort sub-lane: admitted up to effective_cap * (1 - reliable_headroom_pct)
        │     └── at_least_once sub-lane: admitted up to effective_cap
        ├── PressureStateMachine (reused from mailbox/detail/)
        ├── EndpointCircuitBreaker (connection-failure driven)
        └── atomic Counts (control_msgs, control_bytes, data_msgs, data_bytes)
```

**Admission flow** for `try_send()`:

1. Check `EndpointCircuitBreaker` — if Open, return `EndpointCircuitOpen`.
2. Classify message as control or data based on `TypeTag` range (system tags
   `0x00-0x7F` are control; user tags `0x80+` are data).
3. Compute effective capacity: `effective = limits.max_messages - limits.control_lane_reserve`.
4. Control lane: admit if `control_messages < limits.control_lane_reserve` OR
   total messages < `limits.max_messages`. Control frames are never rejected in
   favor of data frames.
5. Data lane:
   - If at-least-once: admit if `data_messages < effective`.
   - If best-effort: admit if `data_messages < effective * (1 - reliable_headroom_pct)`.
6. Byte budget: same logic applied to `*_bytes` atomics vs `limits.max_bytes`.
   A message must pass BOTH count and byte checks.
7. On reject: update pressure state, emit rate-limited backpressure signal if
   state transitioned, return `EndpointBackpressure` with `retry_after` hint.
8. On accept: atomically increment counts, enqueue, update pressure state.

**Dequeue flow** (on `flush_pending()` or connection-ready drain):

1. Pop from front (control lane first if both non-empty).
2. Atomically decrement counts.
3. Update pressure state with post-dequeue ratio.
4. If pressure clears (transition to Normal), emit a clearing signal.

### 4.2 EndpointOutboundQueue

```cpp
namespace hpactor::net {

struct EndpointOutboundLimits {
    size_t max_messages = 1000;
    size_t max_bytes = 16 * 1024 * 1024;  // 16 MiB
    size_t control_lane_reserve = 64;
    double reliable_headroom_pct = 0.20;
};

struct EndpointOutboundCounts {
    std::atomic<size_t> control_messages{0};
    std::atomic<size_t> control_bytes{0};
    std::atomic<size_t> data_messages{0};
    std::atomic<size_t> data_bytes{0};
};

class EndpointOutboundQueue {
public:
    explicit EndpointOutboundQueue(const EndpointOutboundLimits& limits);

    // Returns EnqueueResult. On success, message is queued.
    // On rejection, returns EndpointBackpressure or EndpointCircuitOpen.
    EnqueueResult try_enqueue(PendingMessage msg, DeliveryMode mode);

    // Dequeue returns nullopt if both lanes empty. Prefers control lane.
    std::optional<PendingMessage> try_dequeue();

    // Snapshot for metrics/CLI — consistent read of all counters.
    EndpointOutboundCounts snapshot() const;

    // Pressure state machine access.
    mailbox::MailboxPressureState pressure_state() const;

    // Current depth ratio (0.0–1.0+). Used to drive PressureStateMachine.
    double depth_ratio() const;

    size_t total_messages() const;
    size_t total_bytes() const;

private:
    EndpointOutboundLimits limits_;
    EndpointOutboundCounts counts_;

    // Two-lane storage: control deque + data deque.
    // Lock-free MPSC enqueue, single-consumer dequeue (event loop thread).
    std::deque<PendingMessage> control_lane_;
    std::deque<PendingMessage> data_lane_;

    mailbox::detail::PressureStateMachine pressure_;
};

} // namespace hpactor::net
```

**Byte tracking** uses `encoded.size()` from the `StreamBuffer` payload —
zero-overhead, no deep accounting. Slightly over-counts (includes frame headers)
but the error is bounded and conservative.

**Thread safety**: `try_enqueue()` is called from arbitrary actor/scheduler
threads. `try_dequeue()` is called from the event loop thread during
`flush_pending()`. The atomic counters provide the safety boundary for admission
decisions; the deques themselves are single-consumer. An `std::atomic_flag`
spinlock protects deque push/pop for the case of concurrent `try_enqueue` calls
from different threads. The `snapshot()` method reads each atomic individually —
the returned view is not a consistent point-in-time snapshot, which is
acceptable for metrics and CLI display purposes.

### 4.3 Pressure State Machine

Reuses the existing `mailbox::detail::PressureStateMachine` unchanged:

| State | Trigger | Meaning at endpoint level |
|---|---|---|
| Normal | depth_ratio < high_watermark | Queue is healthy. |
| SoftPressure | depth_ratio ≥ high_watermark | Queue is filling; remote senders should slow down. |
| HardPressure | depth_ratio ≥ critical_watermark | Queue is near exhaustion; best-effort messages are being rejected. |
| Recovering | was ≥ high, now < high but > low | Depth dropping but not yet safe. No new pressure signals. |

Watermarks are TOML-configured (same defaults as mailbox pressure):
- `high_watermark = 0.70`
- `critical_watermark = 0.90`
- `low_watermark = 0.50`

**Push (advisory)**: On state transition to SoftPressure or HardPressure, emit a
rate-limited `BackpressureSignal` control frame to the remote endpoint via the
existing `TypeTag::BackpressureSignalTag` path. Rate limits:

- 1 signal per second per state change.
- 1 signal per 5 seconds while staying in HardPressure (re-reminder).
- Clearing signal (back to Normal) sent immediately, not rate-limited.

**Pull (authoritative)**: `try_send()` → `try_enqueue()` returns
`EnqueueResultCode::EndpointBackpressure` when the data lane rejects. The result
carries a `retry_after` hint derived from current depth ratio and a simple
exponential moving average of drain rate.

**Drain rate EMA**: updated on each dequeue, `rate = alpha * bytes_dequeued +
(1 - alpha) * rate`, with `alpha = 0.2` (TOML-configurable). Used only for
`retry_after` hints; never for admission decisions.

### 4.4 Endpoint Circuit Breaker

```cpp
namespace hpactor::net {

struct EndpointCircuitBreakerConfig {
    size_t failure_threshold = 5;
    std::chrono::milliseconds cooldown{30'000};
    size_t half_open_probe_limit = 1;
};

class EndpointCircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    explicit EndpointCircuitBreaker(const EndpointCircuitBreakerConfig& config);

    // Called by ConnectionPool::on_connection_error().
    void record_failure();

    // Called by ConnectionPool::on_connection_ready().
    void record_success();

    // Called before each try_enqueue(). Returns true if message may proceed.
    // In HalfOpen, only admits up to probe_limit messages.
    bool allow_send();

    // Returns EndpointCircuitOpen if the breaker is Open.
    EnqueueResultCode check_before_send();

    State state() const;

    // Operator override: force-reset to Closed.
    void reset();

private:
    EndpointCircuitBreakerConfig config_;
    std::atomic<State> state_{State::Closed};
    std::atomic<size_t> failure_count_{0};
    std::atomic<size_t> half_open_probes_{0};
    std::chrono::steady_clock::time_point opened_at_;
    std::mutex mutex_;  // protects cooldown timing, not the hot path
};

} // namespace hpactor::net
```

**State machine**:

```
Closed ──(failure_count >= threshold)──→ Open
Open   ──(cooldown elapsed, send arrives)──→ HalfOpen
HalfOpen ──(connection succeeds + probe accepted)──→ Closed
HalfOpen ──(connection fails or probe times out)──→ Open
```

- `record_failure()` increments `failure_count`; if ≥ threshold, transitions
  Closed → Open, records `opened_at_`.
- `record_success()` resets `failure_count` to 0 when in Closed or HalfOpen.
  In HalfOpen, the success also transitions to Closed.
- `allow_send()`:
  - Closed: always returns true.
  - Open: checks if cooldown has elapsed. If yes, transitions to HalfOpen and
    returns true (probe). If no, returns false.
  - HalfOpen: increments `half_open_probes_`, returns true if ≤ probe_limit,
    false otherwise.
- When the breaker is Open or HalfOpen with no remaining probes,
  `check_before_send()` returns `EnqueueResultCode::EndpointCircuitOpen`.

The circuit breaker is **independent** of pressure state. A disconnected endpoint
has an Open circuit but Normal pressure (empty queue). A connected but slow
remote can have HardPressure but a Closed circuit.

### 4.5 ConnectionPool Integration

`ConnectionPool` changes:

1. Replace `std::deque<PendingMessage> pending_messages_` with
   `EndpointOutboundQueue outbound_queue_`.
2. Replace `bool add_pending()` with `EnqueueResult try_enqueue()` that calls
   `outbound_queue_.try_enqueue()`.
3. `try_send()` returns the `EnqueueResult` from `try_enqueue()` instead of a
   bare `bool`.
4. `flush_pending()` calls `outbound_queue_.try_dequeue()` in a loop, preferring
   control frames first.
5. `on_connection_error()` calls `circuit_breaker_.record_failure()`.
6. `on_connection_ready()` calls `circuit_breaker_.record_success()`.
7. `PoolConfig` gains `EndpointOutboundLimits` and
   `EndpointCircuitBreakerConfig` fields, or references them from system config.
8. `PoolStats` gains pressure state, circuit breaker state, and per-lane depth
   fields.

### 4.6 TOML Configuration

```toml
[system.transport.outbound]
# Per-endpoint queue limits
max_queued_messages = 1000
max_queued_bytes = 16777216          # 16 MiB

# Lane configuration
control_lane_reserve = 64            # Hard minimum control frame slots
reliable_headroom_pct = 0.20         # 20% of data lane reserved for at-least-once

# Pressure watermarks
high_watermark = 0.70
critical_watermark = 0.90
low_watermark = 0.50

# Push signal rate limiting
pressure_signal_max_per_sec = 1      # Max state-change signals per sec per endpoint
pressure_signal_linger_secs = 5      # Re-emit interval while staying in HardPressure

# Circuit breaker
circuit_failure_threshold = 5        # Consecutive failures to open circuit
circuit_cooldown_ms = 30000          # Wait before half-open probe
circuit_half_open_probe_limit = 1    # Max messages admitted in HalfOpen

# Drain rate estimation
drain_rate_ema_alpha = 0.2           # Smoothing factor for byte drain rate EMA
```

The parser follows the existing subsystem parser IoC pattern. A new file
`src/config/parsers/transport_outbound_config_parser.cpp` registers itself via
`TomlSystemParserRegistration<TransportOutboundConfigParser>`. The parser uses
`TomlTableView` and does not expose `toml.hpp` in its header. All fields have
defaults as shown above — the section is entirely optional.

### 4.7 Failure Semantics

Two new `EnqueueResultCode` values in the transport domain:

| Code | FailureReason mapping | Retryable? | DLQ reason |
|---|---|---|---|
| `EndpointBackpressure` | `ResourceExhausted` | Yes, with `retry_after` hint | `DeadLetterReason::EndpointBackpressure` |
| `EndpointCircuitOpen` | `RemoteUnavailable` | No | `DeadLetterReason::EndpointCircuitOpen` |

New `DeadLetterReason` values (extending the existing enum):
- `EndpointBackpressure` — data lane at capacity for target endpoint.
- `EndpointCircuitOpen` — circuit breaker open for target endpoint.

`DeadLetterSource::Transport` identifies the originating subsystem (already
exists).

When a message is rejected at the endpoint level and the actor's mailbox policy
includes `DeadLetter`, a `DeadLetterRecord` is created with:
- `reason`: `EndpointBackpressure` or `EndpointCircuitOpen`
- `source`: `DeadLetterSource::Transport`
- `endpoint`: the remote endpoint address (stored in record metadata)
- `trace_context`: preserved from the original message
- `timestamp`: monotonic clock capture at rejection time

### 4.8 Observability

**Metrics** (OpenMetrics gauges and counters):

```
# HELP hpactor_endpoint_outbound_messages Current queued messages per endpoint and lane.
# TYPE hpactor_endpoint_outbound_messages gauge
hpactor_endpoint_outbound_messages{endpoint="10.0.1.5:9090",lane="control"} 3
hpactor_endpoint_outbound_messages{endpoint="10.0.1.5:9090",lane="data"} 142

# HELP hpactor_endpoint_outbound_bytes Current queued bytes per endpoint and lane.
# TYPE hpactor_endpoint_outbound_bytes gauge

# HELP hpactor_endpoint_pressure_state Pressure state per endpoint (0=Normal,1=Recovering,2=SoftPressure,3=HardPressure).
# TYPE hpactor_endpoint_pressure_state gauge

# HELP hpactor_endpoint_circuit_state Circuit breaker state per endpoint (0=Closed,1=Open,2=HalfOpen).
# TYPE hpactor_endpoint_circuit_state gauge

# HELP hpactor_endpoint_send_accepted_total Messages accepted by the outbound queue.
# TYPE hpactor_endpoint_send_accepted_total counter
hpactor_endpoint_send_accepted_total{endpoint="10.0.1.5:9090",mode="best_effort"} 10421
hpactor_endpoint_send_accepted_total{endpoint="10.0.1.5:9090",mode="reliable"} 392

# HELP hpactor_endpoint_send_rejected_total Messages rejected by the outbound queue.
# TYPE hpactor_endpoint_send_rejected_total counter
hpactor_endpoint_send_rejected_total{endpoint="10.0.1.5:9090",reason="queue_full"} 47
hpactor_endpoint_send_rejected_total{endpoint="10.0.1.5:9090",reason="circuit_open"} 218
hpactor_endpoint_send_rejected_total{endpoint="10.0.1.5:9090",reason="shutting_down"} 3

# HELP hpactor_endpoint_backpressure_signals_sent_total Backpressure signals sent per endpoint.
# TYPE hpactor_endpoint_backpressure_signals_sent_total counter

# HELP hpactor_endpoint_circuit_transitions_total Circuit breaker state transitions per endpoint.
# TYPE hpactor_endpoint_circuit_transitions_total counter
```

**CLI commands** (extending the trie-based command tree):

```
/system endpoints                    — list all known endpoints with state, depth, pressure
/system endpoint <ep> show           — detail: limits, per-lane depth, pressure state,
                                       circuit breaker state, recent rejection counts
/system endpoint <ep> circuit reset  — manually close and reset the circuit breaker
                                        (operator override, requires confirmation)
/system endpoint <ep> circuit state  — show circuit breaker state without full endpoint detail
```

Endpoint inspection uses `InspectStateRequest`/`InspectStateReply` — the CLI
never reads transport state directly. The `EndpointOutboundQueue` exposes a
`snapshot()` method that returns an `EndpointOutboundCounts` struct, and the
`EndpointCircuitBreaker` exposes `state()` and `failure_count()`.

**DLQ filtering**: existing `/dlq list` gains an optional `endpoint=<ep>`
filter to show DLQ records originating from a specific endpoint's outbound
rejections.

## 5. Test Plan

### Unit Tests (test_endpoint_outbound_queue.cpp)

| Test | What it verifies |
|---|---|
| `AcceptsUnderLimit` | Messages accepted when count and bytes are under budget. |
| `RejectsAtMessageLimit` | Best-effort messages rejected at data lane message capacity. |
| `RejectsAtByteLimit` | Messages rejected when byte budget exhausted. |
| `ControlLaneAlwaysAdmitted` | Control frames admitted up to control_lane_reserve even when data lane is full. |
| `ControlLaneAboveReserve` | Control frames compete with data above reserve but are not rejected in favor of data. |
| `ReliableHeadroom` | At-least-once messages accepted in reserved headroom when best-effort messages are rejected. |
| `ReliableRejectedBeyondEffective` | At-least-once messages rejected beyond effective capacity. |
| `DequeuePrefersControl` | Control frames dequeued before data frames when both lanes have messages. |
| `PressureTransitionsToSoft` | PressureStateMachine transitions to SoftPressure at high_watermark. |
| `PressureTransitionsToHard` | PressureStateMachine transitions to HardPressure at critical_watermark. |
| `PressureRecoversToNormal` | PressureStateMachine returns to Normal below low_watermark. |
| `ByteTrackingAccurate` | Byte counters reflect encoded.size() accurately through enqueue/dequeue cycle. |
| `SnapshotConsistent` | Snapshot returns atomically consistent view of all counters. |

### Unit Tests (test_endpoint_circuit_breaker.cpp)

| Test | What it verifies |
|---|---|
| `ClosedAllowsAllSends` | All sends admitted when breaker is Closed. |
| `OpensAfterThresholdFailures` | Breaker transitions to Open after N consecutive failures. |
| `OpenRejectsAllSends` | All sends rejected when breaker is Open, returning EndpointCircuitOpen. |
| `CooldownBeforeHalfOpen` | Breaker remains Open until cooldown elapses. |
| `HalfOpenProbeLimit` | Only probe_limit messages admitted in HalfOpen. |
| `HalfOpenSuccessCloses` | Successful connection in HalfOpen resets breaker to Closed. |
| `HalfOpenFailureReopens` | Connection failure in HalfOpen transitions back to Open. |
| `SuccessResetsFailureCount` | record_success() resets failure count in Closed state. |
| `OperatorReset` | reset() forces Closed regardless of current state. |

### Integration Tests (test_connection_pool_outbound.cpp)

| Test | What it verifies |
|---|---|
| `TrySendReturnsBackpressure` | try_send() returns EndpointBackpressure when outbound queue is full. |
| `TrySendReturnsCircuitOpen` | try_send() returns EndpointCircuitOpen when circuit breaker is open. |
| `FlushDrainsControlFirst` | flush_pending() sends control frames before data frames. |
| `PressureSignalEmittedOnTransition` | BackpressureSignal control frame sent when pressure state changes. |
| `PressureSignalRateLimited` | BackpressureSignal not emitted more than max_per_sec. |
| `ClearingSignalOnRecovery` | Signal sent when pressure returns to Normal. |
| `ReconnectResetsCircuit` | Successful reconnect after cooldown closes circuit breaker. |
| `CircuitOpensOnDisconnect` | Connection loss increments failure count and eventually opens breaker. |

### System Tests (test_system_endpoint_backpressure.cpp)

| Test | What it verifies |
|---|---|
| `RemoteSenderReceivesBackpressureSignal` | A remote node receives and processes a BackpressureSignal from an overloaded endpoint. |
| `DLQRecordOnEndpointRejection` | Rejected message with DeadLetter policy creates DLQ record with EndpointBackpressure reason. |
| `MetricsEmittedForRejection` | Rejection increments endpoint_send_rejected_total counter. |
| `CLIListsEndpoints` | `/system endpoints` shows endpoint pressure and circuit state. |
| `CLIResetsCircuit` | `/system endpoint <ep> circuit reset` closes an open breaker. |

## 6. Acceptance Criteria

- [ ] `EndpointOutboundQueue` enforces both message count and byte budget per
  endpoint, with separate limits for control and data lanes.
- [ ] Control frames (backpressure signals, ACK/NACK) are never rejected in favor
  of user data messages when within the control lane reserve.
- [ ] At-least-once messages have reserved headroom beyond the best-effort
  rejection threshold.
- [ ] Per-endpoint pressure state transitions through Normal/SoftPressure/
  HardPressure/Recovering based on configured watermarks with hysteresis.
- [ ] Rate-limited advisory backpressure signals are pushed to remote endpoints
  on pressure state transitions.
- [ ] `try_send()` returns `EndpointBackpressure` (retryable) or
  `EndpointCircuitOpen` (non-retryable) with appropriate metadata.
- [ ] Circuit breaker opens after N consecutive connection failures, transitions
  through cooldown and half-open probe, and closes on successful reconnection.
- [ ] Rejected messages are dead-lettered when the actor's mailbox policy
  includes `DeadLetter`, with `DeadLetterSource::Transport` and endpoint
  metadata preserved.
- [ ] TOML `[system.transport.outbound]` configures all limits, watermarks, and
  circuit breaker parameters with safe defaults.
- [ ] Per-endpoint metrics (depth, bytes, pressure state, circuit state,
  accept/reject counters) are exported via the OpenMetrics `/metrics` endpoint.
- [ ] CLI `/system endpoints` and `/system endpoint <ep> show/reset` commands
  provide operator visibility and control.
- [ ] Existing `send()` fire-and-forget API remains source-compatible.
- [ ] All tests pass under TSAN.

## 7. Dependencies

- **MBX-001** (bounded mailbox admission): provides the admission contract
  pattern that EndpointOutboundQueue follows.
- **MBX-003** (backpressure signal propagation): provides `TypeTag::BackpressureSignalTag`,
  `BackpressureSignal` serialization, and the remote signal delivery path that
  endpoint-level pressure signals use.
- **MBX-005** (priority mailbox lanes): provides the two-lane pattern (system +
  user) that EndpointOutboundQueue's control + data lane design mirrors.
- **MSG-001** (delivery result contract): `EnqueueResult` and `DeliveryStatus`
  are the return types for `try_send()`.

## 8. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Atomic counter overhead on hot path | Two atomic increments per enqueue (count + bytes). Acceptable: these are on the remote-send path, which already crosses a serialization + frame + network boundary. |
| Control lane reserve too small for busy clusters | Default of 64 slots is generous for control traffic. Configurable via TOML. |
| Circuit breaker false-positives on flaky networks | Failure threshold of 5 with 30s cooldown means brief flaps don't open the breaker. Configurable. |
| Byte accounting imprecision (includes frame headers) | Conservative — slightly over-counts, which means we reject slightly earlier than strictly necessary. Safer than under-counting. |
