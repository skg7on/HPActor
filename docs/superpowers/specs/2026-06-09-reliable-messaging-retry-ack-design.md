# Reliable Messaging — Retry Policy & ACK/NACK Control Frames

**Issue**: [#18 — MSG-005](https://github.com/skg7on/HPActor/blob/main/docs/architecture/production/architecture-requirement-backlog.md#L80) / MSG-003 in refined backlog  
**Date**: 2026-06-09  
**Status**: Design spec (pre-implementation)  
**Parent architecture**: [Reliable Messaging Architecture Design](../architecture/production/reliable-messaging-design.md)  
**Dependencies**: Delivery semantics (MSG-001, implemented), DLQ (implemented), DedupCache (implemented)

## 1. Executive Summary

HPActor already has best-effort messaging with typed delivery results, deduplication,
dead-letter queuing, and a `DeliveryPipeline` that handles admission control. This
spec adds the retry layer for opt-in at-least-once delivery: configurable retry
policies, ACK/NACK control frames over the transport, an outbound delivery tracker,
and a `DeliveryReceipt` handle for callers that need to observe the eventual outcome.

The default `send()` path remains fire-and-forget best-effort — unchanged and fast.
Only messages explicitly opted into `AtLeastOnce` or `DurableAtLeastOnce` mode
enter the retry loop.

## 2. Background — Existing Foundation

The following is already implemented and will be reused (not rebuilt):

| Component | Location | Status |
|-----------|----------|--------|
| `DeliveryMode` enum | `msg/delivery_mode.hpp` | Implemented — `BestEffort`, `ObservableBestEffort`, `AtLeastOnce`, `DurableAtLeastOnce` |
| `DeliveryOptions` struct | `msg/enqueue_result.hpp` | Implemented — `delivery_mode`, `message_id`, `flags`, `no_drop` |
| `DeliveryResult` / `DeliveryStatus` | `msg/delivery_result.hpp` | Implemented — 12 status codes, helper functions |
| `DeliveryPipeline` | `mailbox/delivery_pipeline.hpp` | Implemented — dedup → expiration → circuit breaker → enqueue → backpressure |
| `DedupCache` | `adt/dedup_cache.hpp` | Implemented — TTL-based, keyed on (source_node, source_actor, message_id) |
| `DeadLetterQueue` | `mailbox/dead_letter_queue.hpp` | Implemented — bounded, replay, export, policy-based routing |
| `WireFrame` / `ActorMsgFrame` | `msg/frame.hpp`, `protos/hpactor/frame.proto` | Implemented — magic header + length + protobuf body |
| `EnqueueResult` | `msg/enqueue_result.hpp` | Implemented — admission result with depth, capacity, backpressure state |
| `ActorContext::try_send()` / `try_send_with_priority()` / `try_reply()` | `actor/actor_context.hpp` | Implemented |
| Metrics ring buffer | `metrics/metrics_event.hpp` | Implemented — 45 event types |
| RPC `PendingCall` retry | `rpc/rpc_channel.hpp` | Implemented — simple retry pattern; not reused directly |

## 3. Scope

### In scope (this issue)

1. **RetryPolicy** — configurable value type: max attempts, per-attempt timeout, backoff algorithm, jitter.
2. **AckFrame / NackFrame** — protobuf control frames on the transport for delivery acknowledgement.
3. **OutboundDeliveryTracker** — per-node map of `MessageId → PendingSend`, drives retry timers.
4. **DeliveryReceipt** — `std::future`-like handle returned by `try_send()` for tracked modes.
5. **Integration** — hook retry/ACK into `DeliveryPipeline`, `ActorContext::try_send()`, transport decode, scheduler tick.
6. **DLQ exhaustion** — retries exhausted → `DeadLetterRecord` with new `RetryExhausted` reason.
7. **Observability** — 6 new metric event types, `/reliable` CLI commands.
8. **DurableDeliveryStore stub** — abstract interface, no adapters implemented.

### Out of scope (follow-up issues)

- `InMemoryDeliveryStore` and `FileDeliveryStore` adapter implementations.
- Durable outbox/inbox replay after process restart.
- `WireEnvelope` protocol negotiation in the handshake (backward-compat gate).
- Per-actor retry policy TOML config (uses system default only until TOML parser IoC lands).

## 4. Architecture & Components

### 4.1 New Files

```
include/hpactor/msg/
├── retry_policy.hpp              ← RetryPolicy, RetryBackoff enum
├── delivery_receipt.hpp          ← DeliveryReceipt (future-like handle)
└── outbound_delivery_tracker.hpp ← OutboundDeliveryTracker, PendingSend

src/msg/
└── outbound_delivery_tracker.cpp ← tracker state machine + retry loop

protos/hpactor/frame.proto        ← add AckFrame, NackFrame, WireEnvelope

include/hpactor/msg/
└── durable_delivery_store.hpp    ← DurableDeliveryStore (abstract, no adapters)
```

### 4.2 Component Responsibilities

| Component | Role | Owned by |
|-----------|------|----------|
| `RetryPolicy` | Immutable value: max attempts, timeout, backoff, jitter | `DeliveryOptions` (per-message) |
| `AckFrame` / `NackFrame` | Protobuf control frames; decoded at transport layer | Transport decode path |
| `OutboundDeliveryTracker` | Map `MessageId → PendingSend`; drives retry timers; resolves `DeliveryReceipt` on ACK/NACK/exhaustion | `ActorSystem` (one per node) |
| `DeliveryReceipt` | Move-only handle wrapping shared promise; callers poll, `co_await`, or discard | Returned by `try_send()` |
| `DurableDeliveryStore` | Abstract persistence interface (stub only) | (not used until adapters land) |

### 4.3 Data Flow — Local Send, AtLeastOnce

```
try_send(target, msg, opts)
    │
    ▼
DeliveryPipeline::deliver_with_result()         ← dedup, expiry, enqueue
    │  returns Accepted
    ▼
OutboundDeliveryTracker::track(msg_id, deadline, policy)
    │  creates PendingSend + DeliveryReceipt
    │
    ├── ACK arrives ──▶ resolve receipt → Accepted ✓
    │
    ├── NACK arrives ─▶ if retryable → retry after backoff
    │                   if non-retryable → DLQ → resolve receipt ✗
    │
    └── timeout ──▶ if attempts remain → retry
                    if exhausted → DLQ → resolve receipt ✗
```

### 4.4 Data Flow — Remote Send, AtLeastOnce

```
Sender node                                    Receiver node
───────────                                    ─────────────
try_send() → OutboundTracker.track()
           → Transport::send(frame)
           ─────────────────────────────────▶  Transport::recv()
                                               WireEnvelope decode:
                                                 data_frame → DeliveryPipeline::try_deliver()
                                                 │ Accepted → send AckFrame(msg_id)
                                                 │ Rejected → send NackFrame(msg_id, reason, retry_after)
           ◀─────────────────────────────────  Transport::send(ack/nack)
OutboundTracker::on_ack(msg_id)
    → resolve receipt → Accepted
```

**ACK timing**: ACK is sent after mailbox admission, not after user handler completion.
This proves the message entered the receiver's runtime without coupling sender progress
to handler latency.

## 5. Type Designs

### 5.1 RetryPolicy

```cpp
namespace hpactor::msg {

enum class RetryBackoff : uint8_t {
    Fixed,        // delay = initial_backoff every retry
    Linear,       // delay = initial_backoff × attempt_number
    Exponential,  // delay = initial_backoff × 2^attempt_number
};

struct RetryPolicy {
    uint8_t max_attempts = 1;                    // 1 = try once, no retry
    std::chrono::milliseconds per_attempt_timeout{5'000};
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{30'000};
    RetryBackoff backoff = RetryBackoff::Exponential;
    bool jitter = true;                          // ±25% random jitter

    [[nodiscard]] bool is_enabled() const noexcept { return max_attempts > 1; }

    /// Compute the delay before the next retry.
    /// attempt_number is 1-based (1st retry = attempt 2).
    [[nodiscard]] std::chrono::milliseconds
    backoff_delay(uint8_t attempt_number) const noexcept;
};

} // namespace hpactor::msg
```

`RetryPolicy` is added to `DeliveryOptions` as `std::optional<RetryPolicy> retry_policy`.
When absent, `AtLeastOnce` mode uses a system-default policy (max 5 attempts, 5s timeout,
exponential backoff 100ms–30s with jitter).

### 5.2 DeliveryReceipt

```cpp
namespace hpactor::msg {

/// Move-only handle for the eventual outcome of a tracked delivery.
/// Returned by try_send() when DeliveryMode >= AtLeastOnce.
class DeliveryReceipt {
public:
    DeliveryReceipt() = default;
    ~DeliveryReceipt() = default;

    // Move-only
    DeliveryReceipt(DeliveryReceipt&&) noexcept = default;
    DeliveryReceipt& operator=(DeliveryReceipt&&) noexcept = default;

    /// Returns true when the final result is available (non-blocking).
    [[nodiscard]] bool ready() const noexcept;

    /// Block until the result is available.
    /// Only call from a blocking-actor or non-actor thread.
    [[nodiscard]] mailbox::DeliveryResult get() const;

    /// Non-blocking try-get. Returns std::nullopt if not ready.
    [[nodiscard]] std::optional<mailbox::DeliveryResult>
    try_get() const noexcept;

    /// Register a callback invoked when the result arrives.
    /// Callback runs on the scheduler thread — keep it fast.
    void on_complete(std::function<void(mailbox::DeliveryResult)> callback);

    /// Cancel tracking. The runtime stops retrying.
    /// Resolves with DeliveryStatus::Cancelled if not already resolved.
    void cancel();

    /// The message id this receipt tracks.
    [[nodiscard]] MessageId message_id() const noexcept;

private:
    friend class OutboundDeliveryTracker;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace hpactor::msg
```

### 5.3 OutboundDeliveryTracker

```cpp
namespace hpactor::msg {

class OutboundDeliveryTracker {
public:
    struct PendingSend {
        MessageId msg_id;
        StreamBuffer serialized_frame;           // pre-serialized, reused on retry
        EndPoint remote_endpoint;
        ActorAddress sender;
        ActorAddress target;
        RetryPolicy policy;
        uint8_t retry_count = 0;
        uint64_t deadline_ns;                    // absolute, monotonic clock
        uint64_t next_retry_ns = 0;              // absolute; 0 = awaiting first ACK
        DeliveryReceipt receipt;
        std::optional<TraceContext> trace;       // for retry span linkage
    };

    /// Construct with access to transport, DLQ, and the scheduler's timer wheel.
    OutboundDeliveryTracker(/* deps injected */);
    ~OutboundDeliveryTracker();

    // Non-copyable, non-movable (owned by ActorSystem)
    OutboundDeliveryTracker(const OutboundDeliveryTracker&) = delete;
    OutboundDeliveryTracker& operator=(const OutboundDeliveryTracker&) = delete;

    /// Start tracking a new send. Serializes the frame once at track time.
    [[nodiscard]] DeliveryReceipt
    track(WireFrame frame, EndPoint remote, RetryPolicy policy,
          uint64_t deadline_ns);

    /// Called by transport when an AckFrame arrives.
    void on_ack(MessageId msg_id, EndPoint from_endpoint);

    /// Called by transport when a NackFrame arrives.
    /// reason_code maps to DeliveryStatus; retry_after_ms is sender hint.
    void on_nack(MessageId msg_id, EndPoint from_endpoint,
                 uint32_t reason_code, uint32_t retry_after_ms);

    /// Poll retry timers — called from scheduler tick.
    void process_retries(uint64_t now_ns);

    /// Cancel all pending for a disconnected endpoint.
    void cancel_endpoint(EndPoint endpoint, DeliveryStatus reason);

    /// Cancel a specific send by message id.
    void cancel(MessageId msg_id);

    /// Number of currently pending sends.
    [[nodiscard]] size_t pending() const noexcept;

    /// Snapshot for CLI introspection.
    [[nodiscard]] std::vector<PendingSend> snapshot() const;

private:
    // Internal; protected by mutex for concurrent access from transport
    // callbacks + scheduler tick.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hpactor::msg
```

### 5.4 DurableDeliveryStore (Stub)

```cpp
namespace hpactor::msg {

/// Persistence adapter for durable at-least-once delivery.
/// No adapters implemented in this issue — stubbed for DurableAtLeastOnce
/// to degrade to in-memory AtLeastOnce behavior.
class DurableDeliveryStore {
public:
    virtual ~DurableDeliveryStore() = default;

    virtual result<void> put_outbox(const PendingSend& record) = 0;
    virtual result<void> mark_outbox_complete(MessageId id) = 0;
    virtual result<std::vector<PendingSend>> load_pending_outbox() = 0;
    virtual result<void> put_inbox(MessageId id, uint64_t ttl_ns) = 0;
    virtual result<bool> seen_inbox(MessageId id) = 0;
};

} // namespace hpactor::msg
```

## 6. Wire Protocol

### 6.1 Protobuf Changes

```protobuf
// protos/hpactor/frame.proto — additions

message AckFrame {
  uint64 message_id = 1;
  hpactor.PbActorAddress sender = 2;       // who sent the original message
  uint64 sender_node_id = 3;               // receiver's node id
}

message NackFrame {
  uint64 message_id = 1;
  hpactor.PbActorAddress sender = 2;
  uint64 sender_node_id = 3;
  NackReason reason = 4;
  uint32 retry_after_ms = 5;               // 0 = sender decides backoff
}

enum NackReason {
  NACK_UNSPECIFIED = 0;
  NACK_MAILBOX_FULL = 1;           // retryable — sender backs off
  NACK_ACTOR_DEAD = 2;             // non-retryable → DLQ
  NACK_EXPIRED = 3;                // non-retryable → DLQ
  NACK_REJECTED_BY_POLICY = 4;     // non-retryable → DLQ
  NACK_DUPLICATE = 5;              // already seen → treat as ACK
}

// Envelope wrapper for frame type dispatch
message WireEnvelope {
  oneof payload {
    ActorMsgFrame data_frame = 1;
    AckFrame ack_frame = 2;
    NackFrame nack_frame = 3;
  }
}
```

### 6.2 Wire Format

```
Current:  [magic 4B "HPAC"][length 4B][ActorMsgFrame protobuf]
New:      [magic 4B "HPAC"][length 4B][WireEnvelope protobuf]
```

### 6.3 Backward Compatibility

`WireEnvelope` wrapping is gated on protocol negotiation. Peers that do not advertise
the `reliable_messaging` feature flag continue to send/receive raw `ActorMsgFrame`.
The transport decode path checks the negotiated feature set per connection and
falls back to direct `ActorMsgFrame` decode for older peers.

If the sender uses `AtLeastOnce` but the peer does not support ACK/NACK, the
`OutboundDeliveryTracker` resolves the receipt with `DeliveryStatus::Accepted`
on successful transport send (degraded to fire-and-track without remote feedback).

### 6.4 NackReason → DeliveryStatus Mapping

| NackReason | DeliveryStatus | Sender action |
|-----------|---------------|--------------|
| `NACK_MAILBOX_FULL` | `MailboxFull` | Retry with backoff; honor `retry_after_ms` if non-zero |
| `NACK_ACTOR_DEAD` | `ActorDead` | Non-retryable → DLQ immediately |
| `NACK_EXPIRED` | `Expired` | Non-retryable → DLQ |
| `NACK_REJECTED_BY_POLICY` | `RejectedByPolicy` | Non-retryable → DLQ |
| `NACK_DUPLICATE` | `Duplicate` | Treat as ACK (receiver already has it) |

## 7. Integration Points

### 7.1 ActorContext::try_send()

When `DeliveryOptions::delivery_mode >= AtLeastOnce` and `RetryPolicy::is_enabled()`:

1. Call `DeliveryPipeline::deliver_with_result()`.
2. If `Accepted`: call `tracker->track()` and return `DeliveryReceipt`.
3. If not `Accepted`: return the failure `DeliveryResult` directly (no tracking needed).

The existing `send()` overloads are unchanged — they ignore the receipt.

### 7.2 Transport Decode Path

After `WireEnvelope` protobuf decode:

- `data_frame` variant → existing `ActorMsgFrame` processing → `DeliveryPipeline`.
- `ack_frame` variant → `OutboundDeliveryTracker::on_ack()`.
- `nack_frame` variant → `OutboundDeliveryTracker::on_nack()`.

### 7.3 Scheduler Tick

`OutboundDeliveryTracker::process_retries(now_ns)` is called from the system scheduler
tick (same path that drives timer wheel expiry). For each `PendingSend` where
`next_retry_ns <= now_ns`:

1. If `retry_count >= max_attempts`: push `DeadLetterRecord` with reason `RetryExhausted`,
   resolve receipt with `TransportError`, remove entry.
2. Otherwise: re-send the pre-serialized frame via `Transport::send()`, increment
   `retry_count`, compute `next_retry_ns` from backoff policy.

### 7.4 DeliveryResult — New Status

Add to `DeliveryStatus` enum:

```cpp
Cancelled = 12,  // Delivery tracking was cancelled by caller.
```

Also add `Cancelled` to `is_retryable()` (returns `false`) and `to_failure_reason()`
(maps to `FailureReason::Cancelled`). Add `FailureReason::Cancelled` if not already
present in the enum.

### 7.5 DeadLetterQueue — New Reason

Add to `DeadLetterReason` enum:

```cpp
RetryExhausted = 18,  // Reliable delivery retries exhausted.
```

Map `RetryExhausted` to `FailureReason::Timeout` in the `failure_reason()` switch.

### 7.6 DeliveryOptions Extension

Add to `DeliveryOptions`:

```cpp
std::optional<RetryPolicy> retry_policy;  // absent → use system default
```

## 8. Observability

### 8.1 Metrics — New MetricEventType Entries

| Event | Code | Trigger |
|-------|------|---------|
| `kReliableTracked = 46` | — | PendingSend added to tracker |
| `kReliableAckReceived = 47` | DeliveryStatus | AckFrame processed |
| `kReliableNackReceived = 48` | DeliveryStatus | NackFrame processed |
| `kReliableRetry = 49` | attempt_number | Frame re-sent after timeout |
| `kReliableExhausted = 50` | total_attempts | Max retries reached → DLQ |
| `kReliableCancelled = 51` | — | DeliveryReceipt::cancel() called |

### 8.2 CLI Commands

```
/reliable outbox                    — list pending sends (msg_id, target, attempt, next_retry)
/reliable outbox <msg_id>           — show full detail for one pending send
/reliable cancel <msg_id>           — cancel tracking for a send
/reliable stats                     — pending_count, acks, nacks, retries, exhausted, cancelled
```

### 8.3 Logs

- Retry attempt: structured log with `message_id`, `attempt`, `next_retry_ns`.
- Retry exhaustion: structured log with `message_id`, `total_attempts`, `deadline_ns`.
- NACK received: structured log with `message_id`, `reason`, `retry_after_ms`.

## 9. Testing Strategy

All tests use `scheduler_threads = 0` to avoid timing assumptions. Messages and retry
triggers are injected directly.

### 9.1 Unit Tests

| Test | What it verifies |
|------|-----------------|
| `RetryPolicy::is_enabled()` with `max_attempts=1` returns false | Default no-retry for best-effort |
| `RetryPolicy::is_enabled()` with `max_attempts=3` returns true | Enabled detection |
| `RetryPolicy::backoff_delay()` Fixed returns constant | Fixed backoff math |
| `RetryPolicy::backoff_delay()` Exponential returns delay × 2^n | Exponential backoff math |
| `RetryPolicy::backoff_delay()` clamped to `max_backoff` | Backoff ceiling |
| `RetryPolicy` with jitter produces values within ±25% | Jitter bounds |
| `AckFrame` round-trip encode/decode | Protobuf serialization |
| `NackFrame` round-trip encode/decode with `NackReason` | Protobuf serialization |
| `WireEnvelope` oneof dispatch: data_frame vs ack_frame vs nack_frame | Envelope routing |
| `DeliveryReceipt::ready()` false before resolution | Unresolved state |
| `DeliveryReceipt::try_get()` returns `std::nullopt` before resolution | Poll semantics |
| `DeliveryReceipt::get()` blocks until resolved | Blocking semantics |

### 9.2 Integration Tests

| Test | What it verifies |
|------|-----------------|
| `try_send()` BestEffort → no receipt, no tracking | BestEffort unchanged |
| `try_send()` AtLeastOnce → returns valid `DeliveryReceipt` | Receipt creation |
| `track()` then `on_ack()` → receipt resolves `Accepted` | ACK completion |
| `track()` then `on_nack(MAILBOX_FULL)` with retry-after → retry scheduled | NACK retry |
| `track()` then `on_nack(ACTOR_DEAD)` → receipt resolves `ActorDead` | Non-retryable NACK |
| `track()` with `max_attempts=3`, all timeout → receipt resolves `TransportError` | Retry exhaustion |
| Retry exhaustion → `DeadLetterRecord` with `RetryExhausted` pushed to DLQ | DLQ integration |
| `cancel_endpoint()` → all pending for that endpoint resolve `RemoteUnavailable` | Peer disconnect |
| `DeliveryReceipt::cancel()` → receipt resolves `Cancelled` | Manual cancel |
| `process_retries()` re-sends frame with incremented attempt counter | Retry resend |
| NACK with `retry_after_ms` overrides backoff when shorter | NACK timing hint |
| `DedupCache::is_duplicate()` true → `DeliveryPipeline` returns `Duplicate` | Receiver dedup |
| BestEffort send path unchanged (no tracking, no receipt) | Backward compat |

### 9.3 New Test Files

```
tests/unit/msg/test_retry_policy.cpp
tests/unit/msg/test_delivery_receipt.cpp
tests/unit/msg/test_outbound_delivery_tracker.cpp
tests/unit/msg/test_ack_nack_frames.cpp       ← protobuf round-trip
```

## 10. Acceptance Criteria

Per the architecture backlog (MSG-003):

- [ ] Retry policy is configurable per-message via `DeliveryOptions::retry_policy`.
- [ ] `ActorContext::try_send()` returns `DeliveryReceipt` for `AtLeastOnce` and `DurableAtLeastOnce` modes.
- [ ] ACK after mailbox admission resolves the receipt with `Accepted`.
- [ ] NACK with retryable reason schedules a retry with backoff.
- [ ] NACK with non-retryable reason fast-fails to DLQ.
- [ ] Retry exhaustion (max_attempts reached) creates a `DeadLetterRecord`.
- [ ] Receiver `DedupCache` suppresses duplicate deliveries within the TTL window.
- [ ] `OutboundDeliveryTracker` cleans up all pending sends on endpoint disconnect.
- [ ] `DeliveryReceipt::cancel()` stops retries and resolves immediately.
- [ ] Default `send()` (BestEffort) path is unchanged — no tracking overhead.
- [ ] New metrics events are emitted for track, ACK, NACK, retry, exhaustion, cancel.
- [ ] CLI `/reliable` commands show pending state and stats.
- [ ] All tests use `scheduler_threads = 0` (deterministic, no wall-clock waits).

## 11. Out of Scope (Follow-Up Issues)

1. `InMemoryDeliveryStore` and `FileDeliveryStore` adapter implementations.
2. Durable outbox replay after process restart (requires `load_pending_outbox()` integration).
3. `WireEnvelope` feature negotiation in the transport handshake.
4. Per-actor `RetryPolicy` defaults in TOML topology config.
5. `DurableAtLeastOnce` mode wired end-to-end (currently degrades to in-memory `AtLeastOnce`).
6. `DeliveryReceipt` coroutine awaiter (`co_await receipt`).
7. Backpressure integration: NACK with `retry_after` from mailbox pressure signals.

## 12. Design Decisions Record

| Decision | Rationale |
|----------|----------|
| ACK after admission, not handler completion | Decouples sender progress from handler latency; proves message entered receiver runtime |
| RetryPolicy per-message via `DeliveryOptions`, actor-level default later | Matches existing `DeliveryOptions` pattern; actor default added when TOML IoC lands |
| Transport-level control frames (not system messages) | Lower latency; avoids coupling ACK/NACK to actor scheduler dispatch |
| `DeliveryReceipt` as `std::shared_future`-like handle, not callback-only | Callers can poll, block, or co_await; discard is free |
| Pre-serialize frame once at `track()` time | Avoids re-serialization cost on each retry |
| `WireEnvelope` with `oneof` for frame type dispatch | Clean separation of data and control frames; extensible for future frame types |
