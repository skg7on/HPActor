# Actor Delivery Semantics Architecture Design

## 1. Executive Summary

HPActor needs an explicit message delivery contract before it can claim
industry-strength reliability. Today local sends, remote sends, RPC retries,
remote spawn, supervision messages, and future mailbox overflow paths do not
share a single vocabulary for success, rejection, retry, duplicate handling, or
deadline expiry.

This design defines delivery semantics as a runtime contract. The default actor
send remains asynchronous and best effort, but the runtime gains typed delivery
results, message deadlines, retry policy, deduplication hooks, and observability.
Critical flows can opt into stronger behavior without forcing every actor
message through durable storage.

## 2. Current State

Existing behavior:

- `ActorContext::send()` and `ActorRef::send()` are fire-and-forget.
- `ActorSystem::deliver_local()` returns `void`.
- Remote frames carry sender, receiver, message id, type tag, flags, and
  payload.
- RPC has retry and timeout behavior, but it is separate from general actor
  messaging.
- Missing local actors or dead remote routes may fail silently or collapse into
  generic errors.
- Message priority and deadline parameters exist in some paths but are not a
  full delivery contract.

## 3. Goals

1. Define delivery outcomes for local and remote actor messages.
2. Preserve source compatibility for fire-and-forget `send()`.
3. Add opt-in result-returning APIs for callers that need flow control.
4. Define ordering, deadline, retry, and duplicate semantics.
5. Support future reliable messaging without changing the base API again.
6. Route failed or expired messages to dead-letter handling when configured.
7. Expose delivery outcomes through metrics, logs, traces, and CLI.

## 4. Non-Goals

- Exactly-once delivery across a distributed cluster.
- Global total ordering across actors or nodes.
- Durable persistence for every message.
- Blocking event-based actors while waiting for remote ACKs.

## 5. Delivery Classes

### 5.1 Best Effort

Default mode for `send()`.

Contract:

- The runtime attempts delivery once.
- Sender does not receive a delivery result.
- Failure is recorded through metrics/logging/DLQ if enabled.
- Local FIFO ordering is preserved per sender-to-mailbox lane as far as the
  mailbox implementation supports it.

### 5.2 Observable Best Effort

Mode for `try_send()` or internal `try_deliver_local()`.

Contract:

- The runtime attempts delivery once.
- Caller receives `DeliveryResult`.
- No retry is performed by the actor runtime.
- Caller decides whether to retry, slow down, or fail its request.

### 5.3 At-Least-Once

Mode for RPC and future reliable actor messages.

Contract:

- Sender keeps an outbound record until ACK, timeout, or cancellation.
- Receiver may observe duplicates.
- Message must carry stable `MessageId`.
- Receiver-side deduplication is available when enabled.
- Idempotency is required for application correctness.

### 5.4 Durable At-Least-Once

Mode for critical messages.

Contract:

- Sender persists an outbox record before network send.
- Receiver persists an inbox or dedup record before ACK.
- Recovery replays unacknowledged messages.
- Delivery may be delayed by storage health.

## 6. Delivery Result Model

Add a shared enum:

```cpp
enum class DeliveryStatus : uint8_t {
    Accepted,
    NoRoute,
    ActorDead,
    MailboxFull,
    Expired,
    Duplicate,
    RemoteUnavailable,
    RejectedByPolicy,
    SerializationError,
    TransportError,
};

struct DeliveryResult {
    DeliveryStatus status{DeliveryStatus::Accepted};
    ActorAddress target;
    MessageId message_id{};
    uint32_t detail_code{0};
    bool retryable{false};
};
```

`send()` ignores this result by design. `try_send()` exposes it.

## 7. Message Metadata

Every message envelope should have:

- `MessageId`: stable identity for delivery tracking and deduplication.
- `TraceContext`: causal tracing.
- `deadline_ns`: absolute or monotonic deadline.
- `priority`: scheduler and mailbox priority.
- `delivery_mode`: best effort, observable, at least once, durable.
- `attempt`: resend attempt count.
- `flags`: idempotent, system, no-drop, trace-sampled, compressed.

## 8. Ordering Semantics

HPActor should document these rules:

- Per-actor execution remains turn-based: one message is handled at a time per
  event-based actor.
- Local default mailbox preserves accepted FIFO order for the default lane.
- Priority mailboxes may reorder by priority.
- Remote transport preserves order only per connection, not across reconnects,
  retries, or multiple pooled connections.
- At-least-once retry may create duplicates and apparent reorder.
- System control messages may bypass user-message overload policy when marked
  as protected.

## 9. Deadline And Expiry

Each message can carry a deadline. The runtime checks it at:

- Enqueue/admission time.
- Dequeue time before actor execution.
- Retry scheduling.
- Remote receive before local delivery.

Expired messages are not delivered to user handlers. They produce
`DeliveryStatus::Expired` and may enter the DLQ depending on policy.

## 10. Retry And Deduplication

Retry policy belongs to explicit delivery modes, not default `send()`.

Recommended components:

- `OutboundDeliveryTracker`: pending sends by `MessageId`.
- `AckFrame` and `NackFrame`: remote delivery acknowledgement.
- `RetryPolicy`: max attempts, timeout, backoff, jitter.
- `DedupCache`: receiver-side fixed-size or TTL cache keyed by sender and
  message id.
- `DeliveryReceipt`: optional application-visible acknowledgement.

## 11. Observability

Metrics:

- `hpactor_delivery_attempts_total`
- `hpactor_delivery_results_total`
- `hpactor_delivery_latency_seconds`
- `hpactor_delivery_retries_total`
- `hpactor_delivery_duplicates_total`
- `hpactor_delivery_expired_total`

Logs:

- Route failure.
- Expired message.
- Retry exhausted.
- Duplicate suppressed.
- Delivery mode policy rejection.

Traces:

- Add delivery result attributes to send and receive spans.
- Link retry attempts to the original message span.

CLI:

- Inspect recent delivery failures.
- Inspect actor-level delivery policy.
- Inspect dedup cache size and hit count.

## 12. Acceptance Criteria

- Every local and remote delivery path maps failures to `DeliveryStatus`.
- Fire-and-forget API remains source-compatible.
- Result-returning APIs are available for backpressure-aware callers.
- Deadline expiry is enforced before actor handler execution.
- Remote retry and duplicate behavior are documented and measurable.
- DLQ receives undeliverable messages when policy requires it.

