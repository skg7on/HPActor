# MSG-002: Result-Returning Local and Remote Delivery APIs — Design Spec

**Issue**: [#15](https://github.com/skg7on/HPActor/issues/15)
**Subsystem**: Messaging
**Priority**: P0
**Release lane**: Foundation
**Backlog source**: `docs/architecture/production/architecture-requirement-backlog.md#L77`
**Architecture doc**: `docs/architecture/production/actor-delivery-semantics-design.md`

## 1. Executive Summary

HPActor already has a result-returning `try_send()` that returns `EnqueueResult`
— a mailbox-focused admission result. But `EnqueueResult` is an internal type
with mailbox-specific fields (pressure ratio, depth, backpressure reason) that
don't map cleanly to the end-to-end delivery outcome a user cares about. For
remote delivery, the transport layer returns `bool`, discarding all failure
detail.

MSG-002 introduces `DeliveryStatus` and `DeliveryResult` as the unified,
user-facing delivery outcome type for both local and remote paths. It defines a
clear mapping from internal admission results to user-visible delivery status,
extends the transport layer to report richer failure information, and ensures
that every `try_send()` caller can distinguish _accepted_, _no route_, _actor
dead_, _mailbox full_, _expired_, _duplicate_, _remote unavailable_, _rejected
by policy_, _serialization error_, and _transport error_ without reading
mailbox internals.

## 2. What Already Exists

This design builds on MSG-001 infrastructure that is complete and tested.

### 2.1 Delivery Paths (MSG-001 ✅)

| Item | Location | Returns |
|------|----------|---------|
| `void send()` fire-and-forget | `actor_context.hpp`, `actor_ref.hpp`, `actor_proxy.hpp` | `void` |
| `EnqueueResult try_send()` | `actor_context.hpp`, `actor_ref.hpp`, `actor_proxy.hpp` | `EnqueueResult` |
| `send_with_priority()` | `actor_context.hpp` | `void` |
| `try_send_with_priority()` | `actor_context.hpp` | `EnqueueResult` |
| `ActorSystem::deliver_local()` | `actor_system.hpp` | `void` |
| `ActorSystem::try_deliver_local()` | `actor_system.hpp` | `EnqueueResult` |
| `Transport::send()` fire-and-forget | `net/transport.hpp` | `void` |
| `Transport::try_send()` | `net/transport.hpp` | `bool` |

### 2.2 Mailbox Admission Types ✅

| Item | Location |
|------|----------|
| `EnqueueResultCode` (10 values: Accepted, AcceptedWithSoftPressure, Rejected, DroppedNewest, DroppedExisting, ReroutedToDeadLetter, ReroutedToOverflow, MailboxClosed, ActorNotFound, EndpointBackpressure, EndpointCircuitOpen) | `mailbox/mailbox_policy.hpp` |
| `EnqueueResult` struct (code, target, depth, capacity, bytes, byte_capacity, pressure_reason, pressure_state, pressure_ratio, retry_after) | `mailbox/mailbox_policy.hpp` |
| `EnqueueResult::accepted()` / `ok()` / `retryable()` / `failure_reason()` | `mailbox/mailbox_policy.hpp` |
| `failure_reason(EnqueueResultCode)` constexpr mapping | `mailbox/mailbox_policy.hpp` |
| `DeliveryMode` enum (BestEffort, ObservableBestEffort, AtLeastOnce, DurableAtLeastOnce) | `mailbox/delivery_mode.hpp` |
| `DeliveryOptions` struct (delivery_mode, message_id, no_drop, allow_blocking, emit_backpressure, flags) | `mailbox/mailbox_policy.hpp` |

### 2.3 Failure Infrastructure ✅

| Item | Location |
|------|----------|
| `FailureReason` (23 values) | `types/failure_reason.hpp` |
| `FailureEnvelope` | `types/failure_envelope.hpp` |
| `FailureSource` (12 subsystem origins) | `types/failure_reason.hpp` |
| `DeadLetterQueue` with `DeadLetterReason` | `mailbox/dead_letter_queue.hpp` |

### 2.4 Remote Delivery Path ✅

| Step | File | Return |
|------|------|--------|
| `ActorProxy::try_send()` serializes, resolves route, calls `Transport::try_send()` | `src/ref/actor_proxy.cpp:41-124` | `EnqueueResult` (`Accepted` / `Rejected` / `ActorNotFound`) |
| `Transport::try_send()` → `TcpTransport::try_send()` → `ConnectionPool::try_send()` | `src/net/tcp_transport.cpp`, `src/net/connection_pool.cpp` | `bool` |
| DLQ capture on transport rejection, no route, no transport | `src/ref/actor_proxy.cpp:44-56,71-84,108-121` | — |

### 2.5 Observability ✅

| Item | Location |
|------|----------|
| `kDeliveryFailure` metric event emitted on try_deliver_local failure | `src/actor/actor_system.cpp` |
| Structured log warnings on delivery failure | `src/actor/actor_system.cpp` |
| `DeadLetterRecord` with reason, source, trace | `mailbox/dead_letter_queue.hpp` |

## 3. Gap Analysis

### 3.1 No User-Facing Delivery Outcome Type

`EnqueueResult` exposes mailbox internals (pressure_ratio, depth, capacity,
backpressure_reason) that are meaningful to the mailbox subsystem but not to a
user who just wants to know "did my message get delivered?" Conversely, it
lacks fields the user would want (delivery status as a flat enum, whether the
failure is retryable at the application level).

### 3.2 Remote delivery returns `bool` — all failure detail is lost

`Transport::try_send()` returns `bool`. The caller gets `true` (accepted by
transport) or `false` (something went wrong). There is no way to distinguish:

- Connection not established vs. connection lost mid-send
- Outbound queue full vs. endpoint circuit breaker open
- Serialization failure vs. transport shutdown

`ActorProxy::try_send()` does its best to map these to `EnqueueResult`
(`Accepted`, `Rejected`, or `ActorNotFound`), but the mapping is coarse because
the transport provides no detail.

### 3.3 No `DeliveryResult` type matching the architecture design

The architecture design doc defines:

```cpp
enum class DeliveryStatus : uint8_t {
    Accepted, NoRoute, ActorDead, MailboxFull, Expired, Duplicate,
    RemoteUnavailable, RejectedByPolicy, SerializationError, TransportError,
};

struct DeliveryResult {
    DeliveryStatus status{DeliveryStatus::Accepted};
    ActorAddress target;
    MessageId message_id{};
    uint32_t detail_code{0};
    bool retryable{false};
};
```

Neither `DeliveryStatus` nor `DeliveryResult` exists in the codebase.

### 3.4 `EnqueueResultCode` values don't map 1:1 to delivery outcomes

`EnqueueResultCode` mixes operational states (AcceptedWithSoftPressure,
ReroutedToOverflow) with failure states (ActorNotFound, MailboxClosed). Users
shouldn't need to understand mailbox pressure to interpret a delivery result.

### 3.5 No result-returning reply API

`ActorContext::reply()` is fire-and-forget. There is no `try_reply()` returning
a delivery result.

## 4. Goals

1. Define `DeliveryStatus` and `DeliveryResult` as public, user-facing types
   matching the architecture design doc vocabulary.
2. Add `EnqueueResult → DeliveryResult` mapping so internal admission results
   translate to user-visible delivery outcomes without exposing mailbox
   internals.
3. Extend `Transport::try_send()` to return a typed `TransportSendResult`
   instead of `bool`, preserving failure detail.
4. Wire the transport result into `ActorProxy::try_send()` so remote delivery
   failures produce distinct `DeliveryStatus` values.
5. Ensure `try_send()` on `ActorContext`, `ActorRef`, and `ActorProxy` returns
   `DeliveryResult` — the unified, user-facing type.
6. Keep `void send()` fire-and-forget (source compatible).
7. Add `try_reply()` returning `DeliveryResult`.
8. Expose delivery results through metrics, logs, and traces with the shared
   `DeliveryStatus` vocabulary.

## 5. Non-Goals

- End-to-end remote delivery confirmation (remote node reports its admission
  result back to the sender). That requires a response frame protocol and
  belongs to MSG-005 (reliable messaging). For remote `try_send()`, the
  `DeliveryResult` reflects the **local** outcome — what this node can determine
  before the message leaves the process.
- Changing `EnqueueResult` or `EnqueueResultCode`. These remain the internal
  mailbox admission vocabulary. The mapping goes one way: `EnqueueResult` →
  `DeliveryResult`.
- At-least-once ACK/NACK delivery tracking. MSG-005 scope.
- Durable outbox/inbox. MSG-005/DUR-001 scope.
- Batch or streaming protocol results. MSG-004 scope.
- Per-message delivery callback/promise. Future enhancement.

## 6. Design

### 6.1 `DeliveryStatus` Enum

Placed in `include/hpactor/mailbox/delivery_result.hpp`.

```cpp
enum class DeliveryStatus : uint8_t {
    /// Message was accepted for delivery. For local delivery, this means
    /// the mailbox admitted the message. For remote delivery, this means
    /// the transport layer accepted the frame for transmission.
    Accepted = 0,

    /// Message was accepted but the target actor or endpoint is under
    /// pressure. The caller may choose to slow down.
    AcceptedWithPressure = 1,

    /// No route to the target actor or node could be resolved.
    NoRoute = 2,

    /// The target actor is terminated or was never spawned.
    ActorDead = 3,

    /// The target mailbox is at hard capacity and the overflow policy
    /// rejected the message.
    MailboxFull = 4,

    /// The message deadline expired before delivery could complete.
    Expired = 5,

    /// A duplicate message was suppressed by the receiver dedup cache.
    Duplicate = 6,

    /// The remote node or endpoint is not reachable.
    RemoteUnavailable = 7,

    /// The message was rejected by mailbox overflow policy, security
    /// policy, or actor lifecycle gate.
    RejectedByPolicy = 8,

    /// Message serialization failed (protobuf encoding error, payload
    /// too large, or unsupported type tag).
    SerializationError = 9,

    /// Transport-level failure: connection lost, send buffer full,
    /// circuit breaker open, or transport shutting down.
    TransportError = 10,

    /// The local actor system is shutting down and not accepting new
    /// messages.
    ShuttingDown = 11,
};
```

Each `DeliveryStatus` value maps to a canonical `FailureReason` for
observability consistency. The mapping function lives alongside the enum:

```cpp
constexpr FailureReason to_failure_reason(DeliveryStatus s) noexcept;
constexpr bool is_retryable(DeliveryStatus s) noexcept;
constexpr bool is_accepted(DeliveryStatus s) noexcept;
constexpr const char* to_string(DeliveryStatus s) noexcept;
```

### 6.2 `DeliveryResult` Struct

Placed in the same header.

```cpp
struct DeliveryResult {
    DeliveryStatus status{DeliveryStatus::Accepted};
    ActorAddress target;
    MessageId message_id{};
    uint32_t detail_code{0};

    // ── Accessors (match EnqueueResult convention) ──────────────────────

    [[nodiscard]] bool ok() const noexcept {
        return is_accepted(status);
    }
    [[nodiscard]] bool accepted() const noexcept {
        return is_accepted(status);
    }
    [[nodiscard]] bool retryable() const noexcept {
        return is_retryable(status);
    }
    [[nodiscard]] FailureReason failure_reason() const noexcept {
        return to_failure_reason(status);
    }

    // ── Factory from internal types ────────────────────────────────────

    /// Build a DeliveryResult from a local mailbox EnqueueResult.
    static DeliveryResult from_enqueue(const mailbox::EnqueueResult& er,
                                       const ActorAddress& target_addr,
                                       MessageId msg_id = {});

    /// Build a DeliveryResult from a remote transport send result.
    static DeliveryResult from_transport(TransportSendResult tsr,
                                         const ActorAddress& target_addr,
                                         MessageId msg_id = {});
};
```

**Design rationale for separating `DeliveryResult` from `EnqueueResult`:**

`EnqueueResult` carries operational fields (depth, capacity, pressure_ratio,
retry_after) that are meaningful for backpressure coordination between the
scheduler, mailbox, and overflow handlers. Exposing those to user code creates
a coupling hazard — users might depend on pressure internals that change across
releases.

`DeliveryResult` carries only what a user needs to decide their next action:
was it accepted? If not, what went wrong? Is it worth retrying?

Advanced callers who need pressure detail call `try_deliver_local()` directly
and work with `EnqueueResult`. That path remains available.

**Relationship to `DeliveryMode`:**

| Mode | `try_send()` returns | Notes |
|------|---------------------|-------|
| `BestEffort` | `DeliveryResult` (discarded by `send()`) | `send()` is the normal API |
| `ObservableBestEffort` | `DeliveryResult` | This IS the use case |
| `AtLeastOnce` | `DeliveryResult` for local admission; async ACK for remote | MSG-005 scope |
| `DurableAtLeastOnce` | `DeliveryResult` for local admission; async ACK after persist | MSG-005/DUR-001 scope |

### 6.3 `EnqueueResult` → `DeliveryResult` Mapping

The mapping from internal admission codes to user-visible delivery status is
deterministic and constexpr where possible:

| `EnqueueResultCode` | `DeliveryStatus` | `retryable` |
|---------------------|------------------|-------------|
| `Accepted` | `Accepted` | — |
| `AcceptedWithSoftPressure` | `AcceptedWithPressure` | — |
| `Rejected` | `MailboxFull` | `true` |
| `DroppedNewest` | `MailboxFull` | `true` |
| `DroppedExisting` | `MailboxFull` | `true` |
| `ReroutedToDeadLetter` | `RejectedByPolicy` | `false` |
| `ReroutedToOverflow` | `AcceptedWithPressure` | — |
| `MailboxClosed` | `ActorDead` | `true` |
| `ActorNotFound` | `NoRoute` | `true` |
| `EndpointBackpressure` | `RemoteUnavailable` | `true` |
| `EndpointCircuitOpen` | `RemoteUnavailable` | `false` |

The mapping preserves semantic precision: `AcceptedWithSoftPressure` is not
collapsed into `Accepted` because the caller should know the target is under
load. `ReroutedToOverflow` maps to `AcceptedWithPressure` because the message
was accepted (into the overflow queue), not rejected.

### 6.4 Transport Result Extension

#### 6.4.1 `TransportSendResult` Enum

Placed in `include/hpactor/net/transport.hpp`.

```cpp
enum class TransportSendResult : uint8_t {
    /// Frame was queued for transmission.
    Sent = 0,

    /// No connection to the target endpoint exists.
    NotConnected = 1,

    /// The outbound queue for this endpoint is at capacity.
    QueueFull = 2,

    /// The endpoint circuit breaker is open.
    CircuitOpen = 3,

    /// The frame could not be serialized (encode failed).
    EncodeError = 4,

    /// The transport is shutting down.
    ShuttingDown = 5,

    /// Write to the socket failed (connection lost mid-write).
    WriteError = 6,
};
```

#### 6.4.2 `Transport::try_send()` Signature Change

```cpp
// Before:
virtual bool try_send(const ActorAddress& target,
                      const StreamBuffer& encoded) = 0;

// After:
virtual TransportSendResult try_send(const ActorAddress& target,
                                     const StreamBuffer& encoded) = 0;
```

`Transport::send()` (fire-and-forget) continues to discard the result:

```cpp
virtual void send(const ActorAddress& target, const StreamBuffer& encoded) {
    (void)try_send(target, encoded);
}
```

#### 6.4.3 `TransportSendResult` → `DeliveryStatus` Mapping

| `TransportSendResult` | `DeliveryStatus` |
|-----------------------|------------------|
| `Sent` | `Accepted` |
| `NotConnected` | `RemoteUnavailable` |
| `QueueFull` | `RemoteUnavailable` |
| `CircuitOpen` | `RemoteUnavailable` |
| `EncodeError` | `SerializationError` |
| `ShuttingDown` | `ShuttingDown` |
| `WriteError` | `TransportError` |

### 6.5 API Surface Changes

#### 6.5.1 `ActorContext`

```cpp
// ── Existing (unchanged contract) ──────────────────────────────────────
void send(const ActorAddress& target, TypedMessage msg);
void send(ActorRef& target, TypedMessage msg);
void send_with_priority(const ActorAddress& target, TypedMessage msg,
                        uint8_t priority, int64_t deadline_ns);
void reply(TypedMessage msg);
void reply_with_error(const error& err);

// ── Changed: return type EnqueueResult → DeliveryResult ────────────────
DeliveryResult try_send(const ActorAddress& target, TypedMessage msg,
                         mailbox::DeliveryOptions options = {});

DeliveryResult try_send_with_priority(const ActorAddress& target,
                                       TypedMessage msg,
                                       uint8_t priority,
                                       int64_t deadline_ns,
                                       mailbox::DeliveryOptions options = {});

// ── New ────────────────────────────────────────────────────────────────
DeliveryResult try_reply(TypedMessage msg,
                          mailbox::DeliveryOptions options = {});
```

#### 6.5.2 `ActorRef`

```cpp
// ── Existing (unchanged) ───────────────────────────────────────────────
void send(const ActorAddress& target, TypedMessage msg);

// ── Changed: return type ───────────────────────────────────────────────
DeliveryResult try_send(const ActorAddress& target, TypedMessage msg,
                         mailbox::DeliveryOptions options = {});
```

#### 6.5.3 `ActorProxy`

```cpp
// ── Existing (unchanged) ───────────────────────────────────────────────
void send(const ActorAddress& target, TypedMessage msg);

// ── Changed: return type ───────────────────────────────────────────────
DeliveryResult try_send(const ActorAddress& target, TypedMessage msg,
                         mailbox::DeliveryOptions options = {});
```

Internally, `ActorProxy::try_send()` now maps `TransportSendResult` →
`DeliveryResult` via `DeliveryResult::from_transport()`, replacing the current
`bool` → `EnqueueResult` mapping.

#### 6.5.4 `ActorSystem`

```cpp
// ── Existing (unchanged) — returns EnqueueResult for internal paths ────
mailbox::EnqueueResult try_deliver_local(ActorId target, TypedMessage msg,
                                          uint8_t priority = 0,
                                          int64_t deadline_ns = INT64_MAX,
                                          mailbox::DeliveryOptions options = {});

// ── New: user-facing delivery result ───────────────────────────────────
DeliveryResult deliver_with_result(ActorId target, TypedMessage msg,
                                    uint8_t priority = 0,
                                    int64_t deadline_ns = INT64_MAX,
                                    mailbox::DeliveryOptions options = {});
```

`deliver_with_result()` calls `try_deliver_local()` internally and converts the
`EnqueueResult` to `DeliveryResult` via `DeliveryResult::from_enqueue()`. This
gives non-actor callers (main thread, blocking actors) a clean result type
without pressure internals.

#### 6.5.5 `Transport` Interface

```cpp
// ── Changed: return type bool → TransportSendResult ────────────────────
virtual TransportSendResult
try_send(const ActorAddress& target, const StreamBuffer& encoded) = 0;

// ── Unchanged (delegates to try_send, discards result) ─────────────────
virtual void send(const ActorAddress& target, const StreamBuffer& encoded) {
    (void)try_send(target, encoded);
}
```

All transport implementations (`TcpTransport`, `ConnectionPool`, UDS, future
proactor backends) return `TransportSendResult` instead of `bool`.

#### 6.5.6 `ConnectionPool`

```cpp
// ── Changed: return type ───────────────────────────────────────────────
TransportSendResult try_send(const ActorAddress& target,
                              const StreamBuffer& encoded);
```

#### 6.5.7 `EndpointOutboundQueue`

```cpp
// ── Changed: return type ───────────────────────────────────────────────
TransportSendResult try_enqueue(PendingMessage msg,
                                 mailbox::DeliveryMode mode,
                                 TypeTag type_tag);
```

### 6.6 Reply Result-Returning Variants

`try_reply()` on `ActorContext` follows the same pattern: it captures the
current sender from the active message context and delegates to `try_send()`.

```cpp
DeliveryResult ActorContext::try_reply(TypedMessage msg,
                                        mailbox::DeliveryOptions options) {
    if (!current_sender_) {
        return DeliveryResult{DeliveryStatus::NoRoute, {}, {}, 0};
    }
    return try_send(*current_sender_, std::move(msg), options);
}
```

### 6.7 Remote Delivery Result Semantics

For remote delivery, `DeliveryResult::Accepted` means the **local transport
accepted the frame for transmission**. It does NOT mean the remote node
successfully delivered the message to the target actor.

This is a deliberate scope boundary:

| What `try_send()` can observe locally | Scope |
|---------------------------------------|-------|
| Transport not connected | MSG-002 (this design) |
| Outbound queue full | MSG-002 |
| Circuit breaker open | MSG-002 |
| Frame encoded and queued | MSG-002 |
| Remote node received the frame | MSG-005 (reliable messaging ACK) |
| Remote mailbox admitted the message | MSG-005 |
| Remote actor processed the message | Future (delivery receipt) |

This matches the architecture design doc's contract for observable best-effort:
the runtime attempts delivery once, and the caller receives the local outcome.

## 7. Observability

### 7.1 Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `hpactor_delivery_results_total` | Counter | `status`, `subsystem` | Delivery outcomes by status |
| `hpactor_transport_send_results_total` | Counter | `result` | Transport send outcomes |

Add a `kDeliveryResult` metric event to the existing `MetricEvent` schema so
the metrics ring buffer can carry delivery outcome events alongside mailbox
enqueue/dequeue events.

### 7.2 Logs

Structured log on delivery failure (non-Accepted status):

```
level=WARN msg="delivery_failed" actor_id=<sender> target=<receiver>
    delivery_status=<status> retryable=<bool> message_id=<id>
    trace_id=<trace> detail_code=<code>
```

Use the existing structured logging path (`LogManager` → `LogRingBuffer` →
`LogDrain` → sinks). No new log sink or formatter.

### 7.3 Traces

Add span attributes on the send span:
- `delivery.status`: string value of `DeliveryStatus`
- `delivery.retryable`: bool
- `delivery.target`: ActorId string

Use the existing `TraceManager` → `SpanGuard` path. No new trace exporter or
wire format change.

### 7.4 CLI

Extend the existing `/actor` command group:

```
/actor delivery <actor_id>     — show recent delivery results for an actor
/actor delivery-stats <id>     — aggregated delivery status counts
```

New command handlers in `src/cli/commands/` following the existing
`CommandNode` registration pattern. No new CLI actor or I/O thread.

### 7.5 Dead-Letter Queue

DLQ records already capture `DeadLetterReason` and `DeadLetterSource`. No
schema changes are needed. The mapping from `DeliveryStatus` to
`DeadLetterReason` is used when `try_send()` rejects a message and the actor's
overflow policy routes to DLQ:

| `DeliveryStatus` | `DeadLetterReason` |
|------------------|--------------------|
| `MailboxFull` | `MailboxOverflow` |
| `NoRoute` | `MissingRoute` |
| `ActorDead` | `ActorTerminated` |
| `Expired` | `MessageExpired` |
| `RemoteUnavailable` | `RemoteNodeUnreachable` |
| `TransportError` | `TransportSendFailed` |
| `RejectedByPolicy` | `RejectedByOverflowPolicy` |
| `ShuttingDown` | `SystemShutdown` |

## 8. Configuration

No new TOML configuration is required. `DeliveryMode::ObservableBestEffort` is
already selectable through `DeliveryOptions`. The delivery result API is always
available when the caller uses `try_send()` — it is not gated behind a config
flag.

The `DeliveryStatus` → `FailureReason` mapping is compile-time (constexpr) and
does not require runtime configuration.

## 9. Compatibility

### 9.1 Source Compatibility

`void send()` signatures are unchanged. All existing fire-and-forget call sites
compile without modification.

`try_send()` return type changes from `EnqueueResult` to `DeliveryResult`.
Callers that destructure `EnqueueResult` fields (`.code`, `.depth`,
`.pressure_ratio`, `.retry_after`) will need to update. The following accessor
names are preserved to minimize churn:

| `EnqueueResult` | `DeliveryResult` equivalent |
|-----------------|----------------------------|
| `.ok()` | `.ok()` — same name, same semantics |
| `.accepted()` | `.accepted()` — same name, same semantics |
| `.retryable()` | `.retryable()` — same name, same semantics |
| `.failure_reason()` | `.failure_reason()` — same name, same semantics |
| `.status()` / `.code` | `.status()` — returns `DeliveryStatus` instead of `EnqueueResultCode` |

Callers that need the full `EnqueueResult` (pressure state, depth, retry_after)
should call `ActorSystem::try_deliver_local()` directly.

### 9.2 Binary Compatibility

`DeliveryResult` is a new type. Existing `EnqueueResult` layout is unchanged.
No ABI break for code that only uses `EnqueueResult`.

### 9.3 Wire Compatibility

No frame format changes. `TransportSendResult` is a local enum, not
transmitted. No protobuf schema changes.

## 10. Acceptance Criteria

- [ ] `DeliveryStatus` enum with all 12 values is defined and documented.
- [ ] `DeliveryResult` struct with factory methods and accessors is defined.
- [ ] `EnqueueResult` → `DeliveryResult` mapping is constexpr and covers all
      11 `EnqueueResultCode` values.
- [ ] `TransportSendResult` enum is defined and `Transport::try_send()` returns
      it instead of `bool`.
- [ ] `TcpTransport`, `ConnectionPool`, `EndpointOutboundQueue` return
      `TransportSendResult`.
- [ ] `ActorProxy::try_send()` maps `TransportSendResult` → `DeliveryResult`.
- [ ] `ActorContext::try_send()` returns `DeliveryResult`.
- [ ] `ActorRef::try_send()` returns `DeliveryResult`.
- [ ] `try_reply()` exists on `ActorContext` and returns `DeliveryResult`.
- [ ] `ActorSystem::deliver_with_result()` returns `DeliveryResult`.
- [ ] `void send()` and `void reply()` remain source-compatible.
- [ ] Every `DeliveryStatus` maps to a canonical `FailureReason`.
- [ ] Metrics emit `kDeliveryResult` events with status label.
- [ ] CLI `/actor delivery` and `/actor delivery-stats` commands work.
- [ ] All existing test binaries compile. Tests that destructure
      `EnqueueResult` from `try_send()` are updated.
- [ ] Failing `Transport::try_send()` returns distinct `TransportSendResult`
      values for not-connected, queue-full, circuit-open, and write-error.

## 11. Test Plan

### 11.1 Unit Tests

| Test file | Scope |
|-----------|-------|
| `test_delivery_result.cpp` | `DeliveryStatus` ↔ `FailureReason` mapping, `DeliveryResult::from_enqueue()` for all 11 `EnqueueResultCode` values, `DeliveryResult::from_transport()` for all 7 `TransportSendResult` values, accessor correctness, `is_accepted()`, `is_retryable()`, `to_string()` |
| `test_transport_send_result.cpp` | `TransportSendResult` → `DeliveryStatus` mapping, `to_string()` |
| `test_try_send_local.cpp` | `ActorContext::try_send()` local path returns `DeliveryResult::Accepted`, `NoRoute`, `MailboxFull`, `Expired`, `Duplicate`, `ActorDead` |
| `test_try_send_remote.cpp` | `ActorProxy::try_send()` returns correct `DeliveryResult` for each `TransportSendResult` value (mock transport) |
| `test_try_reply.cpp` | `ActorContext::try_reply()` returns `DeliveryResult`, no-sender case returns `NoRoute` |
| `test_deliver_with_result.cpp` | `ActorSystem::deliver_with_result()` maps `EnqueueResult` → `DeliveryResult` |

### 11.2 Integration Tests

| Test file | Scope |
|-----------|-------|
| `test_delivery_result_integration.cpp` | End-to-end `try_send()` through `ActorRef` → `ActorSystem` → mailbox → `DeliveryResult`, remote `try_send()` through `ActorRef` → `ActorProxy` → transport → `DeliveryResult` |

### 11.3 CLI Tests

| Test file | Scope |
|-----------|-------|
| `test_delivery_commands.cpp` | `/actor delivery <id>`, `/actor delivery-stats <id>` output format and edge cases |

### 11.4 Existing Tests Requiring Update

Tests that call `try_send()` and inspect the returned `EnqueueResult` directly
need updating. The following binaries are affected:

- `test_actor_context` — calls `try_send()`, checks `EnqueueResult::code`
- `test_actor_ref` — calls `try_send()`, checks `EnqueueResult`
- `test_actor_proxy` — calls `try_send()`, checks result
- `test_delivery_mode` — may reference `try_send()` return type
- `test_dedup_cache` — calls `try_deliver_local()`, uses `EnqueueResult`
- Integration tests that pattern-match on `EnqueueResultCode`

Tests that use `try_deliver_local()` directly (internal path) are NOT affected
since that method's return type is unchanged.

## 12. Files Changed

### 12.1 New Files

| File | Purpose |
|------|---------|
| `include/hpactor/mailbox/delivery_result.hpp` | `DeliveryStatus` enum, `DeliveryResult` struct, `to_string()`, `is_accepted()`, `is_retryable()`, `to_failure_reason()`, factory methods |
| `tests/unit/mailbox/test_delivery_result.cpp` | Unit tests for mapping and accessors |
| `tests/unit/net/test_transport_send_result.cpp` | Unit tests for transport result mapping |
| `tests/unit/actor/test_try_reply.cpp` | Unit tests for `try_reply()` |
| `tests/unit/core/test_deliver_with_result.cpp` | Unit tests for `deliver_with_result()` |
| `tests/integration/actor/test_delivery_result_integration.cpp` | End-to-end integration tests |
| `tests/unit/cli/test_delivery_commands.cpp` | CLI delivery command tests |

### 12.2 Modified Files

| File | Change |
|------|--------|
| `include/hpactor/mailbox/mailbox_policy.hpp` | Add `EnqueueResult::to_delivery_result()` method |
| `include/hpactor/net/transport.hpp` | Add `TransportSendResult` enum; change `try_send()` return type |
| `include/hpactor/net/tcp_transport.hpp` | Update `try_send()` return type |
| `include/hpactor/net/connection_pool.hpp` | Update `try_send()` return type |
| `include/hpactor/net/endpoint_outbound_queue.hpp` | Update `try_enqueue()` return type |
| `include/hpactor/net/udp_transport.hpp` | Update `try_send()` return type |
| `include/hpactor/actor_context.hpp` | Change `try_send()` return type; add `try_reply()` |
| `include/hpactor/ref/actor_ref.hpp` | Change `try_send()` return type |
| `include/hpactor/ref/actor_proxy.hpp` | Change `try_send()` return type |
| `include/hpactor/core/actor_system.hpp` | Add `deliver_with_result()` |
| `src/actor/actor_context.cpp` | Update implementations; add `try_reply()` |
| `src/actor/actor_system.cpp` | Add `deliver_with_result()`; update any result handling |
| `src/ref/actor_ref.cpp` | Update `try_send()` implementation |
| `src/ref/actor_proxy.cpp` | Map `TransportSendResult` → `DeliveryResult` |
| `src/net/tcp_transport.cpp` | Return `TransportSendResult` |
| `src/net/connection_pool.cpp` | Return `TransportSendResult` |
| `src/net/endpoint_outbound_queue.cpp` | Return `TransportSendResult` |
| `src/net/udp_transport.cpp` | Return `TransportSendResult` |
| `src/cli/commands/` | Add delivery command handlers |
| `src/metrics/` | Add `kDeliveryResult` metric event type |
| `tests/unit/actor/test_actor_context.cpp` | Update for `DeliveryResult` return type |
| `tests/unit/ref/test_actor_ref.cpp` | Update for `DeliveryResult` return type |
| `tests/unit/ref/test_actor_proxy.cpp` | Update for `DeliveryResult` return type |
| Various test files using `try_send()` | Update result destructuring |

## 13. Dependencies

### 13.1 Upstream (must be complete before MSG-002 starts)

- MSG-001 (delivery semantics foundation) ✅ — `DeliveryMode`, `DeliveryOptions`,
  `try_deliver_local()`, deadline enforcement, dedup cache, `FailureEnvelope`
  are all complete and tested.

### 13.2 Downstream (depends on MSG-002)

- MSG-003 (message deadline/TTL enforcement) — uses `DeliveryResult::Expired`
- MSG-005 (reliable messaging ACK/NACK) — uses `DeliveryResult` for local
  admission and async completion
- MBX-003 (backpressure signal propagation) — uses `AcceptedWithPressure` to
  decide when to emit pressure signals
- MBX-006 (remote outbound queue limits) — uses `TransportSendResult::QueueFull`

### 13.3 Release Slice Alignment

MSG-002 is in Release Slice A (Production Foundation), alongside:
- MSG-001 ✅
- MBX-001 ✅, MBX-002 ✅, MBX-003 ✅
- AR-001 ✅, AR-002 ✅, AR-003 ✅
- OBS-001, OBS-002, OPS-001

## 14. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `try_send()` return type change breaks downstream code | Medium | Medium | Preserve `.ok()`, `.accepted()`, `.retryable()`, `.failure_reason()` accessor names; update all in-repo call sites |
| `TransportSendResult` values don't cover all real failure modes | Low | Medium | Start with 7 values covering the known failure paths in `ConnectionPool` and `TcpTransport`; add values as new paths are discovered |
| `DeliveryResult` loses useful `EnqueueResult` detail (pressure_ratio, retry_after) | Low | Low | Advanced callers use `try_deliver_local()` directly; document this path |
| Performance regression from constructing `DeliveryResult` on every send | Low | Low | `DeliveryResult` is trivially copyable, stack-allocated, same size as `EnqueueResult` |
