# MSG-001: Actor Delivery Semantics — Detailed Design Spec

## 1. Issue Reference

- **Issue**: [#14](https://github.com/skg7on/HPActor/issues/14) — MSG-001
- **Subsystem**: Messaging
- **Priority**: P0
- **Status**: Missing → In Design
- **Architecture doc**: `docs/architecture/production/actor-delivery-semantics-design.md`

## 2. Executive Summary

HPActor has a fire-and-forget `send()` and a result-returning `try_send()`, but
no concept of *delivery class* — a sender cannot express "I want at-least-once
delivery for this message." The runtime lacks delivery mode awareness, deadline
enforcement at dequeue time, and a receiver-side deduplication hook.

MSG-001 establishes the delivery semantics contract: four delivery classes
represented in the API and config, unified through the already-implemented
`FailureReason`/`FailureEnvelope` infrastructure, and wired into the existing
`try_deliver_local()` and `try_send()` paths.

## 3. What Already Exists

This design builds on substantial pre-existing infrastructure. Items marked ✅
are complete and tested; this spec does not re-design them.

### 3.1 Failure Infrastructure ✅

| Item | Location | Status |
|------|----------|--------|
| `FailureReason` enum (23 values, 10 ranges) | `types/failure_reason.hpp` | 803 tests pass |
| `FailureSource` enum (12 subsystem origins) | `types/failure_reason.hpp` | ✅ |
| `retryable(FailureReason)` constexpr | `types/failure_reason.hpp` | ✅ |
| `to_string(FailureReason)` | `types/failure_reason.hpp` + `src/types/failure_reason.cpp` | ✅ |
| `FailureEnvelope` struct | `types/failure_envelope.hpp` | ✅ |
| `make_failure_envelope()` factory | `types/failure_envelope.hpp` | ✅ |
| `EnqueueResultCode → FailureReason` mapping | `mailbox/mailbox_policy.hpp` | ✅ |
| `EnqueueResult::failure_reason()` | `mailbox/mailbox_policy.hpp` | ✅ |

### 3.2 Delivery Paths ✅

| Item | Location | Status |
|------|----------|--------|
| `void send()` fire-and-forget | `actor_context.hpp`, `actor_context.cpp` | ✅ |
| `EnqueueResult try_send()` | `actor_context.hpp`, `actor_context.cpp` | ✅ |
| `send_with_priority()` | `actor_context.hpp`, `actor_context.cpp` | ✅ |
| `try_send_with_priority()` | `actor_context.hpp`, `actor_context.cpp` | ✅ |
| `ActorSystem::deliver_local()` | `actor_system.hpp`, `actor_system.cpp` | ✅ |
| `ActorSystem::try_deliver_local()` | `actor_system.hpp`, `actor_system.cpp` | ✅ |
| `FailureEnvelope` built on `try_deliver_local` failure | `actor_system.cpp:421-433`, `473-495` | ✅ |
| `kDeliveryFailure` metric event emitted | `actor_system.cpp:429-431` | ✅ |
| Dead-letter capture on ActorNotFound + OverflowPolicy | `actor_system.cpp:401-467` | ✅ |

### 3.3 Mailbox / Policy Infrastructure ✅

| Item | Location | Status |
|------|----------|--------|
| `DeliveryOptions` (no_drop, allow_blocking, message_id, flags) | `mailbox/mailbox_policy.hpp` | ✅ |
| `OverflowPolicy` (8 policies) | `mailbox/mailbox_policy.hpp` | ✅ |
| `BackpressureMode` / `BackpressureSignal` | `mailbox/mailbox_policy.hpp` | ✅ |
| `MailboxConfig` | `mailbox/mailbox_policy.hpp` | ✅ |
| Bounded mailbox capacity | `mailbox/` | ✅ |
| DLQ actor | `mailbox/` | ✅ |

### 3.4 What MSG-001 Must Add

The pre-existing infrastructure handles *what happens when delivery fails*, but
does not define *what delivery guarantees a sender is requesting*. The gaps:

1. **No `DeliveryMode` concept.** A sender cannot annotate a message with
   "at-least-once" vs "best-effort." `DeliveryOptions` has flags and priority but
   no delivery class.

2. **No deadline enforcement at dequeue time.** Messages carry `deadline_ns` in
   `MailboxEnvelopeMeta`, and it's checked at enqueue, but there is no check
   before actor handler execution. Expired messages can still be dispatched.

3. **No receiver dedup infrastructure.** At-least-once delivery requires
   receiver-side deduplication. No dedup cache interface exists.

4. **No retry contract for general sends.** RPC has retry, but the general
   `send()` path does not. At-least-once mode needs retry.

5. **Delivery contract is architecture-doc-only.** The four delivery classes
   (best-effort, observable best-effort, at-least-once, durable at-least-once)
   exist in the architecture doc but are not represented in the runtime.

## 4. Goals

1. Define `DeliveryMode` as a first-class concept in the API and config.
2. Wire `DeliveryMode` into `DeliveryOptions` so senders can opt into stronger
   guarantees.
3. Enforce message deadlines at dequeue time (before handler execution).
4. Add a receiver-side `DedupCache` interface with a TTL-based implementation.
5. Document the runtime contract for each delivery class — what is guaranteed,
   what is best-effort, and what failure modes exist.
6. Keep `void send()` fire-and-forget with best-effort semantics (source
   compatible).
7. Expose delivery outcomes through existing metrics, logs, and CLI surfaces.

## 5. Non-Goals

- **MSG-005**: ACK/NACK control frames and retry scheduler for reliable
  messaging. This spec defines the *contract* for at-least-once but the retry
  mechanism is MSG-005.
- **MSG-002**: A separate `DeliveryResult` return type distinct from
  `EnqueueResult`. MSG-001 makes `EnqueueResult` delivery-mode-aware; MSG-002
  adds richer result APIs.
- **MSG-006**: Full receiver dedup cache implementation with wire protocol
  integration. This spec defines the interface and a TTL-based in-memory
  implementation; wire integration is MSG-005/006.
- **MSG-008**: Durable at-least-once with persistent outbox/inbox. This spec
  defines the delivery mode enum value and contract, but durable storage is
  MSG-008.
- **Remote path changes**: Remote `try_send()` already delegates to
  `ActorProxy::try_send()`. Transport-level delivery mode propagation (encoding
  `DeliveryMode` in frames) is a follow-on; local delivery is the focus.
- **Exactly-once delivery**.
- **Global ordering across actors**.

## 6. Design

### 6.1 `DeliveryMode` Enum

```cpp
namespace hpactor::mailbox {

/// Delivery guarantee requested by the sender.
///
/// The mode governs whether the runtime tracks the message after send,
/// retries on transient failure, suppresses duplicates at the receiver,
/// and persists the message for crash recovery.
enum class DeliveryMode : uint8_t {
    /// Fire-and-forget. The runtime attempts delivery once. No delivery
    /// result is returned to the sender. Failure is recorded via
    /// metrics/logging/DLQ when those subsystems are enabled. Local FIFO
    /// order is preserved per sender-to-mailbox lane.
    BestEffort = 0,

    /// Single delivery attempt with a result returned to the caller.
    /// The caller decides whether to retry, slow down, or fail its
    /// request. No automatic retry is performed by the runtime.
    ObservableBestEffort = 1,

    /// The sender keeps an outbound record until ACK, timeout, or
    /// cancellation. The receiver may observe duplicates. Messages must
    /// carry a stable MessageId. Receiver-side deduplication is
    /// available when enabled. Caller must ensure handler idempotency.
    AtLeastOnce = 2,

    /// At-least-once delivery with durable outbox/inbox persistence.
    /// The sender persists an outbox record before network send. The
    /// receiver persists an inbox/dedup record before ACK. Recovery
    /// replays unacknowledged messages. Delivery may be delayed by
    /// storage health.
    DurableAtLeastOnce = 3,
};

/// Human-readable name for metrics labels, log keys, and CLI.
constexpr const char* to_string(DeliveryMode mode) noexcept {
    switch (mode) {
        case DeliveryMode::BestEffort:          return "best_effort";
        case DeliveryMode::ObservableBestEffort: return "observable_best_effort";
        case DeliveryMode::AtLeastOnce:         return "at_least_once";
        case DeliveryMode::DurableAtLeastOnce:  return "durable_at_least_once";
    }
    return "best_effort";
}

} // namespace hpactor::mailbox
```

### 6.2 Updated `DeliveryOptions`

`DeliveryOptions` gains a `delivery_mode` field. The default is `BestEffort` to
preserve source compatibility.

```cpp
struct DeliveryOptions {
    DeliveryMode delivery_mode = DeliveryMode::BestEffort;  // NEW
    bool no_drop = false;
    bool allow_blocking = false;
    bool emit_backpressure = true;
    uint64_t message_id = 0;
    uint32_t flags = 0;
};
```

When `delivery_mode` is `ObservableBestEffort` or stronger, the runtime MUST
populate `message_id` if the caller did not provide one (a monotonic counter per
sender). This enables delivery tracking even when the caller doesn't explicitly
set a message id.

### 6.3 Delivery Contract Per Mode

#### 6.3.1 BestEffort

- **API**: `send()` and `try_send()` with `DeliveryMode::BestEffort` (default).
- **Attempts**: 1.
- **Result**: `send()` returns void. `try_send()` returns `EnqueueResult`.
- **Retry**: None.
- **Dedup**: None.
- **Persistence**: None.
- **Deadline**: Checked at enqueue only. Expired messages are dropped.
- **Ordering**: Per-sender-to-mailbox FIFO (within priority lane).
- **Failure visibility**: Metrics counter, structured log, optional DLQ.
- **Use case**: Telemetry, heartbeats, stateless notifications.

#### 6.3.2 ObservableBestEffort

- **API**: `try_send()` with `DeliveryMode::ObservableBestEffort`.
- **Attempts**: 1.
- **Result**: `EnqueueResult` with full `FailureReason` mapping.
- **Retry**: Caller-driven. The runtime does not retry.
- **Dedup**: None.
- **Persistence**: None.
- **Deadline**: Checked at enqueue and dequeue.
- **Ordering**: Same as BestEffort.
- **Failure visibility**: Full `FailureEnvelope` built on every failure path.
- **Use case**: Request/response where the caller has its own retry logic,
  backpressure-aware producers.

#### 6.3.3 AtLeastOnce

- **API**: `try_send()` with `DeliveryMode::AtLeastOnce`.
- **Attempts**: Configurable max retries (default 3).
- **Result**: `EnqueueResult` reflects final outcome after retries are
  exhausted.
- **Retry**: Runtime-managed retry with exponential backoff. Retry policy is
  per-message or per-actor config.
- **Dedup**: Receiver-side dedup cache keyed on (sender_node, sender_actor,
  message_id). TTL-based, configurable window.
- **Persistence**: In-memory outbox only (non-durable). Lost on crash.
- **Deadline**: Checked at enqueue, dequeue, and before each retry.
- **Ordering**: Retries may create duplicates and apparent reorder. Caller must
  ensure idempotency.
- **Failure visibility**: `FailureEnvelope` on final exhaustion. DLQ record
  with `FailureReason::RetryExhausted`.
- **Use case**: Configuration updates, workflow state transitions, command
  dispatch where loss is unacceptable but crash recovery is not required.

**Note**: The retry scheduler and ACK/NACK wire protocol are deferred to
MSG-005. MSG-001 defines the `AtLeastOnce` enum value, wires it through
`DeliveryOptions`, and adds the infrastructure hooks (dedup cache, deadline
enforcement). The retry loop itself is MSG-005.

#### 6.3.4 DurableAtLeastOnce

- **API**: `try_send()` with `DeliveryMode::DurableAtLeastOnce`.
- **All AtLeastOnce guarantees**, plus:
- **Persistence**: Outbox record persisted before send. Inbox/dedup record
  persisted before ACK. Recovery replays unacknowledged messages.
- **Storage**: Pluggable `DurableDeliveryStore` interface.
- **Use case**: Payment transactions, contract acceptance, audit-significant
  state transitions.

**Note**: Full durable store implementation is deferred to MSG-008. MSG-001
defines the enum value and the `DurableDeliveryStore` interface contract.

### 6.4 Message Deadline Enforcement

Currently, `deadline_ns` is checked at enqueue time (inside
`try_deliver_local` / `try_push`). Expired messages are rejected before entering
the mailbox. But a message that is *accepted* into the mailbox may sit in the
queue for an arbitrary time before the actor processes it.

MSG-001 adds a dequeue-time deadline check.

#### 6.4.1 Check Location

The check is performed in the scheduler dispatch path, immediately before
calling `execute_actor()` or the actor's `receive()`:

```
mailbox->try_pop() → envelope
    │
    ├── deadline_ns < now()  →  drop message
    │       ├── build FailureEnvelope {Expired, ...}
    │       ├── emit kDeliveryFailure metric
    │       ├── log warning
    │       ├── DLQ if policy requires
    │       └── loop to try_pop() again
    │
    └── deadline_ns >= now() → deliver to actor handler
```

#### 6.4.2 Implementation

The check lives in `HybridScheduler` (or the `WorkerThread` dispatch loop),
where the mailbox is popped. It does not live inside `try_push()` because that's
already covered:

- `src/sched/hybrid_scheduler.cpp` — in the worker run loop after `try_pop()`.
- For non-scheduler dispatch (e.g., `scheduler_threads = 0`), the check is in
  the direct dispatch path.

A new free function encapsulates the check:

```cpp
namespace hpactor::mailbox {

/// Check whether a dequeued message has expired.
///
/// Returns true if the message deadline has passed. When true, the
/// caller should drop the message, build a FailureEnvelope with
/// FailureReason::Expired, and emit observability signals.
///
/// Thread safety: constexpr, lock-free. Callable from any thread.
[[nodiscard]] constexpr bool is_expired(int64_t deadline_ns,
                                         uint64_t now_ns) noexcept {
    return deadline_ns >= 0 &&
           static_cast<uint64_t>(deadline_ns) < now_ns;
}

} // namespace hpactor::mailbox
```

### 6.5 Receiver Dedup Cache

At-least-once delivery produces duplicate messages at the receiver. The receiver
needs a bounded, TTL-based dedup cache keyed on `(sender_node, sender_actor,
message_id)`.

#### 6.5.1 Interface

```cpp
namespace hpactor::mailbox {

/// Bounded receiver-side deduplication cache.
///
/// Keyed on (source endpoint, source actor, message id). Entries expire
/// after a configurable TTL. When the cache is full, oldest entries are
/// evicted — this is safe because eviction causes at most one spurious
/// duplicate delivery, which at-least-once semantics already allow.
class DedupCache {
public:
    struct Config {
        size_t max_entries = 1024 * 64;    // 64K entries default
        uint64_t ttl_ns = 300'000'000'000; // 5 minutes default
    };

    explicit DedupCache(Config cfg);
    ~DedupCache();

    // Non-copyable, movable.
    DedupCache(const DedupCache&) = delete;
    DedupCache& operator=(const DedupCache&) = delete;
    DedupCache(DedupCache&&) noexcept;
    DedupCache& operator=(DedupCache&&) noexcept;

    /// Check-and-set: returns true if the key was already seen (duplicate),
    /// false if the key is new (and inserts it).
    ///
    /// Thread safety: safe for concurrent callers. Uses internal
    /// synchronization.
    [[nodiscard]] bool is_duplicate(const CommunicationEndpoint& source_node,
                                     ActorId source_actor,
                                     MessageId message_id) noexcept;

    /// Remove expired entries. Called periodically by the owning actor
    /// or a background timer.
    void purge_expired(uint64_t now_ns) noexcept;

    /// Current entry count (approximate — may be stale).
    [[nodiscard]] size_t size() const noexcept;

    /// Number of duplicate hits since creation.
    [[nodiscard]] uint64_t duplicate_hits() const noexcept;

    /// Number of insertions (non-duplicates) since creation.
    [[nodiscard]] uint64_t insertions() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hpactor::mailbox
```

#### 6.5.2 Integration Point

The dedup cache is checked in `try_deliver_local()` when the message's
`DeliveryMode` is `AtLeastOnce` or `DurableAtLeastOnce`, BEFORE the mailbox
`try_push()`. If the message is a duplicate, the function returns
`EnqueueResult{EnqueueResultCode::Accepted}` (the sender gets a successful
ACK-like result) and emits a `kDeliveryDuplicate` metric event.

The dedup cache is owned by the `ActorSystem` and is distinct from any
future durable inbox. One cache serves all local actors.

#### 6.5.3 Lookup Key

The dedup key for local delivery is:

```
(source endpoint, source actor id, message id)
```

For local sends, `source endpoint` is the local node's endpoint. This avoids
collisions between messages from different nodes that happen to have the same
`(actor_id, message_id)` pair.

### 6.6 Updated `try_deliver_local()` Dispatch

The updated flow with delivery mode awareness:

```
ActorContext::try_send(target, msg, options)
    │
    ├── options.delivery_mode == BestEffort
    │       └── try_deliver_local(target, msg, ..., options)
    │           ├── registry lookup → NoRoute + DLQ + metric
    │           ├── deadline check (enqueue) → Expired + DLQ + metric
    │           └── mailbox->try_push() → result
    │
    ├── options.delivery_mode == ObservableBestEffort
    │       └── try_deliver_local(target, msg, ..., options)
    │           ├── (same as BestEffort, but deadline checked at
    │           │    BOTH enqueue and dequeue)
    │           └── returns EnqueueResult — caller inspects and acts
    │
    ├── options.delivery_mode == AtLeastOnce
    │       └── try_deliver_local(target, msg, ..., options)
    │           ├── dedup check → duplicate → Accepted (ACK-like)
    │           ├── registry lookup → NoRoute + DLQ + metric
    │           ├── deadline check (enqueue) → Expired + DLQ + metric
    │           ├── deadline check (dequeue) → Expired + DLQ + metric
    │           └── mailbox->try_push() → result
    │           [retry loop: MSG-005]
    │
    └── options.delivery_mode == DurableAtLeastOnce
            └── (AtLeastOnce path + outbox persistence: MSG-008)
```

### 6.7 API Source Compatibility

| Existing API | MSG-001 behavior | Breaking? |
|---|---|---|
| `void send(addr, msg)` | BestEffort, unchanged | No |
| `void send(addr, tag, proto)` | BestEffort, unchanged | No |
| `EnqueueResult try_send(addr, msg)` | BestEffort (default), unchanged return type | No |
| `EnqueueResult try_send(addr, msg, opts)` | Mode from `opts.delivery_mode` | No |
| `void send_with_priority(addr, msg, pri, dl)` | BestEffort, unchanged | No |
| `EnqueueResult try_send_with_priority(...)` | BestEffort (default), unchanged | No |

The only API addition is the `delivery_mode` field in `DeliveryOptions`, which
defaults to `BestEffort`. Every existing call site compiles unchanged.

### 6.8 TOML Config

Per-actor delivery defaults are configurable in TOML:

```toml
[[actor]]
name = "payment-processor"
type = "PaymentActor"

[actor.delivery]
default_mode = "at_least_once"
max_retries = 3
dedup_window_ms = 300_000
```

The `[actor.delivery]` table is parsed by a new subsystem parser in
`src/config/parsers/delivery_config_parser.cpp`. Fields:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `default_mode` | string | `"best_effort"` | Default delivery mode for sends from this actor |
| `max_retries` | uint32 | 3 | Max retry attempts for AtLeastOnce (MSG-005) |
| `retry_backoff_ms` | uint32 | 100 | Initial backoff (MSG-005) |
| `retry_backoff_max_ms` | uint32 | 10000 | Max backoff cap (MSG-005) |
| `dedup_window_ms` | uint64 | 300000 | Dedup cache TTL for received messages |
| `dedup_max_entries` | uint64 | 65536 | Dedup cache capacity |

### 6.9 Observability

#### 6.9.1 Metrics (new)

| Metric | Labels | Purpose |
|--------|--------|---------|
| `hpactor_delivery_attempts_total` | `mode`, `result` | Total delivery attempts by mode and outcome |
| `hpactor_delivery_duplicates_total` | — | Messages suppressed by dedup cache |
| `hpactor_delivery_expired_total` | `check_point` (enqueue/dequeue) | Messages dropped due to deadline expiry |

Existing metrics that already serve delivery observability:

| Metric | Status |
|--------|--------|
| `hpactor_failures_total` (via `kDeliveryFailure` events) | ✅ already emitted |
| `hpactor_mailbox_admission_total` | ✅ already emitted |

#### 6.9.2 Logs (new events)

| Event | Level | Fields |
|-------|-------|--------|
| `delivery_expired` | WARNING | message_id, deadline_ns, now_ns, actor_id |
| `delivery_duplicate` | DEBUG | message_id, source, actor_id |
| `delivery_mode_mismatch` | WARNING | requested_mode, actual_mode, reason |

Existing log events that already serve delivery observability:

| Event | Status |
|-------|--------|
| `delivery_failure` (actor_system.cpp:488) | ✅ already emitted |

#### 6.9.3 CLI

The existing `/actor inspect <id>` command gains a delivery section:

```
delivery:
  default_mode: at_least_once
  pending_outbox: 3
  dedup_cache_size: 152
  dedup_duplicate_hits: 7
  dedup_insertions: 12043
```

New commands (follow-on, not blocking MSG-001):
- `/delivery stats [--mode at_least_once]` — aggregate delivery statistics.
- `/delivery pending [--actor <id>]` — pending outbox messages.

### 6.10 Source Layout

```
include/hpactor/mailbox/
    delivery_mode.hpp           // NEW: DeliveryMode enum + to_string
    dedup_cache.hpp             // NEW: DedupCache class
    mailbox_policy.hpp          // MODIFIED: DeliveryOptions gains delivery_mode

include/hpactor/types/
    failure_reason.hpp          // UNCHANGED
    failure_envelope.hpp        // UNCHANGED

src/mailbox/
    dedup_cache.cpp             // NEW: DedupCache implementation

src/actor/
    actor_system.cpp            // MODIFIED: try_deliver_local gains dedup
                                //           check for AtLeastOnce

src/sched/
    hybrid_scheduler.cpp        // MODIFIED: dequeue-time deadline check

src/config/parsers/
    delivery_config_parser.cpp  // NEW: TOML [actor.delivery] parser

tests/unit/mailbox/
    test_delivery_mode.cpp      // NEW: DeliveryMode enum + to_string tests
    test_dedup_cache.cpp        // NEW: DedupCache tests

tests/integration/actor/
    test_delivery_semantics.cpp // NEW: end-to-end delivery mode tests
```

## 7. Phased Implementation Plan

### Phase 1: Delivery Mode + API Contract (this issue)

1. Add `DeliveryMode` enum and `to_string()` to
   `include/hpactor/mailbox/delivery_mode.hpp`.
2. Add `delivery_mode` field to `DeliveryOptions` in
   `include/hpactor/mailbox/mailbox_policy.hpp`.
3. Wire `DeliveryMode` through `try_send()` → `try_deliver_local()`. For
   `AtLeastOnce`: check message_id is set (auto-generate if not), check dedup
   cache. For `DurableAtLeastOnce`: same + placeholder for outbox persistence.
4. Document the contract for each mode in the header doc comments.
5. Add `mailbox::is_expired()` free function for dequeue-time deadline check.
6. Add dequeue-time deadline enforcement in the scheduler dispatch path.
7. Add `DedupCache` interface and TTL-based implementation.
8. Wire dedup cache into `try_deliver_local()` for `AtLeastOnce` and
   `DurableAtLeastOnce` modes.
9. Add `hpactor_delivery_*` metric events and aggregator dispatch.
10. Add unit tests for `DeliveryMode`, `is_expired()`, `DedupCache`.
11. Add integration tests for each delivery mode through
    `try_send()` → `try_deliver_local()`.
12. Add `[actor.delivery]` TOML config parser.

### Phase 2: MSG-002 — Richer Result APIs (follow-on)

- Add `DeliveryResult` struct with full `FailureEnvelope`.
- Add `try_send_result()` returning `DeliveryResult` alongside `EnqueueResult`.
- Remote path delivery result propagation.

### Phase 3: MSG-005 — Retry + ACK/NACK (follow-on)

- `RetryPolicy` struct and `RetryScheduler`.
- `AckFrame` / `NackFrame` wire protocol.
- Outbound delivery tracker for pending at-least-once sends.
- End-to-end retry loop.

### Phase 4: MSG-008 — Durable Delivery (follow-on)

- `DurableDeliveryStore` interface with `InMemoryDeliveryStore` impl.
- Outbox/inbox persistence.
- Recovery replay on restart.

## 8. Compatibility

### 8.1 Preserved

- `void send()` fire-and-forget — unchanged.
- `EnqueueResult try_send()` — unchanged return type.
- `DeliveryOptions` existing fields — unchanged positions and defaults.
- `EnqueueResultCode` enum — unchanged.
- `FailureReason` / `FailureEnvelope` — unchanged.
- All existing test code — compiles without modification.

### 8.2 Modified

- `DeliveryOptions` gains one field (`delivery_mode`). The struct is
  aggregate-initialized in most call sites; the new field's default
  (`BestEffort = 0`) means zero-initialized structs get the correct mode.
- `try_deliver_local()` gains dedup-check logic for modes above
  `ObservableBestEffort`. BestEffort path is unchanged.
- Scheduler dispatch loop gains a deadline check before handler execution.

### 8.3 New

- `include/hpactor/mailbox/delivery_mode.hpp` — new public header.
- `include/hpactor/mailbox/dedup_cache.hpp` — new public header.
- `src/mailbox/dedup_cache.cpp` — new translation unit.
- `src/config/parsers/delivery_config_parser.cpp` — new parser.
- Test files listed in §6.10.

## 9. Acceptance Criteria

- [ ] `DeliveryMode` enum with four values is defined and documented.
- [ ] `DeliveryOptions::delivery_mode` defaults to `BestEffort`.
- [ ] `void send()` is source-compatible and remains best-effort.
- [ ] `try_send()` with `DeliveryMode::BestEffort` behaves identically to
  current `try_send()`.
- [ ] `try_send()` with `DeliveryMode::ObservableBestEffort` returns
  `EnqueueResult` with full `FailureReason` on every failure path.
- [ ] `try_send()` with `DeliveryMode::AtLeastOnce` checks the dedup cache
  before mailbox admission.
- [ ] `DedupCache::is_duplicate()` correctly identifies duplicates and
  non-duplicates under concurrent access.
- [ ] `DedupCache::purge_expired()` removes entries older than TTL.
- [ ] Dequeue-time deadline check prevents expired messages from reaching
  actor handlers.
- [ ] Expired messages produce `FailureReason::Expired` with a
  `FailureEnvelope`.
- [ ] Duplicate messages produce a metric event and return `Accepted` to the
  sender.
- [ ] `[actor.delivery]` TOML config is parsed and applied.
- [ ] All existing 803 tests continue to pass.
- [ ] New tests cover: `DeliveryMode` enum, `is_expired()`, `DedupCache`
  (insert, duplicate, expire, concurrent), dequeue deadline enforcement,
  delivery mode dispatch through `try_deliver_local()`.
- [ ] `hpactor_delivery_*` metrics are emitted and aggregatable.

## 10. Test Plan

### 10.1 Unit Tests (`tests/unit/mailbox/test_delivery_mode.cpp`)

- `to_string()` round-trip for all four values.
- `DeliveryMode` is `uint8_t` and fits in `DeliveryOptions` without padding
  change.
- Default-constructed `DeliveryOptions` has `BestEffort` mode.

### 10.2 Unit Tests (`tests/unit/mailbox/test_dedup_cache.cpp`)

- Insert → not duplicate.
- Insert same key → duplicate.
- Insert different keys → not duplicates.
- Different source nodes with same (actor, message_id) → not duplicates.
- `purge_expired()` removes entries older than TTL.
- `purge_expired()` does not remove entries within TTL.
- Concurrent `is_duplicate()` from multiple threads.
- `size()`, `duplicate_hits()`, `insertions()` counters.
- Eviction when at capacity (oldest entry evicted → not duplicate on re-insert).

### 10.3 Unit Tests (`tests/unit/mailbox/test_is_expired.cpp`)

- `deadline_ns < now_ns` → expired.
- `deadline_ns >= now_ns` → not expired.
- `deadline_ns == -1` (no deadline) → not expired.
- `deadline_ns == 0` edge case.

### 10.4 Integration Tests (`tests/integration/actor/test_delivery_semantics.cpp`)

- `try_send()` with `BestEffort` → accepted, no dedup check.
- `try_send()` with `ObservableBestEffort` → accepted, `EnqueueResult` returned.
- `try_send()` with `AtLeastOnce` → accepted, dedup cache entry created.
- `try_send()` with `AtLeastOnce`, same message twice → second is duplicate.
- `try_send()` with `AtLeastOnce` to dead actor → `NoRoute` in `EnqueueResult`.
- `try_send()` with `AtLeastOnce` to full mailbox → `RejectedByPolicy`.
- Dequeue-time expiry: enqueue message with short deadline, wait, scheduler
  processes → message never reaches handler, `Expired` metric emitted.
- `send()` (void) with any mode → compiles and runs, no API breakage.

## 11. References

- [Actor Delivery Semantics Architecture](actor-delivery-semantics-design.md) — top-level architecture doc.
- [Structured Failure Envelope Design](structured-failure-envelope-design.md) — `FailureReason` / `FailureEnvelope` design.
- [Dead-Letter Queue Design](dead-letter-queue-design.md) — DLQ record format.
- [Reliable Messaging Design](reliable-messaging-design.md) — MSG-005 retry/ACK/NACK design.
- [Production Reliability Plane](production-reliability-plane.md) — overall roadmap.
- [Architecture Requirement Backlog](architecture-requirement-backlog.md#L76) — MSG-001 entry.
