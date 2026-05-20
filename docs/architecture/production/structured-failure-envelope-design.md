# Structured Failure Envelope Architecture Design

## 1. Executive Summary

HPActor has three separate reason-code vocabularies and no shared failure
carrier. `EnqueueResultCode` handles mailbox admission, the `error` class
carries opaque `uint32_t` codes, and the architecture docs define separate
`DeliveryStatus` and `DeadLetterReason` enums that overlap in meaning but sit in
different subsystems. When a send fails because the target actor is dead,
today's caller sees `ActorNotFound` from the mailbox path, `actor_not_found`
from `errors::`, and a DLQ record with a third code from a future enum.

This design introduces a canonical `FailureReason` enum and a `FailureEnvelope`
struct shared by actor send, ask, RPC, spawn, DLQ, and tracing. Existing result
types (`EnqueueResult`, `error`, spawn error codes) continue to work but map to
the shared vocabulary so every failure is traceable with the same metadata:
reason, actor id, message id, trace id, retryable flag, timestamp, subsystem
source, and detail.

## 2. Current State

### 2.1 Implemented

`EnqueueResultCode` in `mailbox/mailbox_policy.hpp` — mailbox admission results:

| Code | Meaning |
|------|---------|
| `Accepted` | Admitted to mailbox |
| `AcceptedWithSoftPressure` | Admitted but near high watermark |
| `Rejected` | Rejected at capacity |
| `DroppedNewest` | Overflow policy dropped newest |
| `DroppedExisting` | Overflow policy displaced existing |
| `ReroutedToDeadLetter` | Diverted to DLQ |
| `ReroutedToOverflow` | Diverted to overflow queue |
| `MailboxClosed` | Mailbox not accepting (draining/stopped) |
| `ActorNotFound` | Target actor not in registry |

`EnqueueResult` carries `code`, `target`, `depth`, `capacity`,
`pressure_ratio`, `retry_after`, `affected_type`, `affected_message_id`.

`error` class in `types/types.hpp` carries `uint32_t code` and `std::string
message`. Canonical codes live in `errors::` namespace: `unknown`, `actor_down`,
`actor_not_found`, `mailbox_full`, `timeout`, `invalid_argument`, plus HTTP
protocol error codes.

`result<T>` in `types/types.hpp` — variant of value or `error`.

Spawn errors use `uint32_t` constants in `spawn.hpp`.

`TraceContext` in `types/types.hpp` — W3C-compatible `TraceId` (16 bytes),
`SpanId` (8 bytes), `TraceFlags`, version, tracestate.

`MessageId` in `types/types.hpp` — unique per-message identifier.

### 2.2 Designed But Not Implemented

`DeliveryStatus` from `actor-delivery-semantics-design.md`:
`Accepted`, `NoRoute`, `ActorDead`, `MailboxFull`, `Expired`, `Duplicate`,
`RemoteUnavailable`, `RejectedByPolicy`, `SerializationError`, `TransportError`.

`DeadLetterReason` from `dead-letter-queue-design.md`:
`NoRoute`, `ActorDead`, `MailboxFull`, `Expired`, `SerializationError`,
`TransportError`, `RejectedByPolicy`, `NodeUnavailable`, `Quarantined`.

### 2.3 The Gap

```text
send() failure ──► EnqueueResultCode::ActorNotFound
                   errors::actor_not_found
                   (future) DeliveryStatus::NoRoute
                   (future) DeadLetterReason::NoRoute
```

Three disconnected vocabularies. No shared metadata. No way to correlate a
failed send with its trace, or a DLQ record with its original delivery attempt.

## 3. Goals

1. Define a canonical `FailureReason` enum shared by all failure paths.
2. Define a `FailureEnvelope` struct carrying actor id, sender address, receiver
   address, message id, trace id, reason code, retryable flag, timestamp,
   subsystem source, and human-readable detail.
3. Map existing `EnqueueResultCode` and `errors::` codes onto `FailureReason`.
4. Make `try_send()` and future `DeliveryResult` produce `FailureReason`.
5. Make DLQ records carry `FailureReason` and `FailureEnvelope` metadata.
6. Make tracing spans carry failure reason as span attributes.
7. Preserve source compatibility for fire-and-forget `send()` and existing
   `EnqueueResult` users.
8. Ensure every new production failure path returns a precise reason — no
   silent `Unknown`.

## 4. Non-Goals

- Replacing `EnqueueResult` as the mailbox admission return type. `EnqueueResult`
  carries mailbox-specific fields (depth, capacity, pressure_ratio) that do not
  belong in a generic failure envelope.
- Replacing the `error` class. `error` remains a simple code+message for handler
  returns. `error` gains the ability to carry an optional `FailureEnvelope`.
- Changing `void send()` return type. The fire-and-forget path intentionally
  discards delivery results.
- Implementing `DeliveryStatus` or `DeadLetterReason` as separate enums. This
  design supersedes both with `FailureReason`.

## 5. Design

### 5.1 `FailureReason` — Canonical Shared Vocabulary

```cpp
namespace hpactor {

enum class FailureReason : uint8_t {
    // ── Route / addressing ───────────────────────────────────────────
    /// No actor or node found for the target address.
    NoRoute = 0,
    /// Remote node is unreachable (network, partition, down).
    NodeUnavailable = 1,

    // ── Actor lifecycle ──────────────────────────────────────────────
    /// Target actor has terminated.
    ActorDead = 10,
    /// Target actor exists but is not ready to accept messages (Starting,
    /// Draining, Recovering, Hibernating).
    ActorNotReady = 11,
    /// Target actor is quarantined — rejecting all user messages.
    Quarantined = 12,
    /// Circuit breaker is open — calls are blocked until cooldown.
    CircuitOpen = 13,

    // ── Resource limits ──────────────────────────────────────────────
    /// Target actor's mailbox is at capacity.
    MailboxFull = 20,
    /// Remote endpoint's outbound queue is at capacity.
    OutboundQueueFull = 21,
    /// Node-level memory pressure prevents admission.
    MemoryPressure = 22,

    // ── Time ─────────────────────────────────────────────────────────
    /// Message deadline expired before delivery.
    Expired = 30,
    /// Operation timed out (ask/request, RPC, spawn).
    Timeout = 31,

    // ── Policy ───────────────────────────────────────────────────────
    /// Rejected by admission or overflow policy.
    RejectedByPolicy = 40,
    /// Message explicitly dropped by overflow policy (DropNewest /
    /// DropOldest / DropLowestPriority).
    Dropped = 41,
    /// Mailbox is closed (actor draining, stopping, or stopped).
    MailboxClosed = 42,

    // ── Transport / serialization ────────────────────────────────────
    /// Message encode or decode failure.
    SerializationError = 50,
    /// Network-level transport failure (connection lost, reset).
    TransportError = 51,
    /// Frame validation failure (size, malformed, corrupt).
    FrameRejected = 52,

    // ── Deduplication ────────────────────────────────────────────────
    /// Duplicate message suppressed by receiver dedup cache.
    Duplicate = 60,

    // ── Graceful shutdown ────────────────────────────────────────────
    /// Node or actor is draining — new ingress rejected.
    Draining = 70,
    /// Node is shutting down — all user ingress rejected.
    ShuttingDown = 71,

    // ── Reliable messaging ───────────────────────────────────────────
    /// All retry attempts exhausted without ACK.
    RetryExhausted = 80,

    // ── Spawn ────────────────────────────────────────────────────────
    /// Remote spawn failed (codec, permission, node, type).
    SpawnFailed = 90,

    // ── Generic / unknown ────────────────────────────────────────────
    /// Unclassified failure. Every occurrence should be promoted to a
    /// specific reason in a follow-up change.
    Unknown = 255,
};

} // namespace hpactor
```

Design rationale for the numbering:

- Ranges partition the namespace: route (0-9), lifecycle (10-19), resource
  (20-29), time (30-39), policy (40-49), transport (50-59), dedup (60-69),
  shutdown (70-79), reliable (80-89), spawn (90-99), reserved for future
  expansion (100-254), Unknown (255).
- `uint8_t` storage keeps the enum small enough to embed in existing structs
  without padding cost.
- `Unknown = 255` is deliberately the maximum value — it is never a valid
  specific reason and stands out in hex dumps.

### 5.2 `FailureEnvelope` — Shared Failure Carrier

```cpp
namespace hpactor {

struct FailureEnvelope {
    /// Canonical failure reason.
    FailureReason reason{FailureReason::Unknown};

    /// Target actor the failure relates to.
    ActorId actor_id{};

    /// Original sender address.
    ActorAddress sender{};

    /// Intended receiver address.
    ActorAddress receiver{};

    /// Message that failed (zeroed for non-message failures like spawn).
    MessageId message_id{};

    /// Distributed trace context at failure point.
    tracing::TraceContext trace{};

    /// True if the caller can retry with a reasonable chance of success.
    bool retryable{false};

    /// Monotonic timestamp (nanoseconds) when the failure was recorded.
    uint64_t timestamp_ns{0};

    /// Which subsystem produced this failure.
    FailureSource source{FailureSource::ActorRuntime};

    /// Human-readable detail for logs and operator inspection.
    /// Bounded to 256 bytes to keep the envelope stack-friendly.
    std::array<char, 256> detail{};
    uint8_t detail_len{0};
};

} // namespace hpactor
```

### 5.3 `FailureSource` — Subsystem Origin

```cpp
enum class FailureSource : uint8_t {
    ActorRuntime,    // Actor send/reply/spawn paths
    Mailbox,         // Mailbox admission
    Rpc,             // RPC channel
    Transport,       // Network transport (TCP, TLS, frame)
    Discovery,       // Service discovery (registrar, gossip)
    Scheduler,       // Scheduling/timing
    Config,          // Config validation/bootstrap
    Security,        // Authentication/authorization
    DurableStore,    // Durable state / event store
    Supervision,     // Supervision / restart
    Cluster,         // Cluster membership / sharding
    Unknown,         // Unspecified source
};
```

### 5.4 <reason, source> pairs

The same `FailureReason` can arise from different subsystems. The combination of
`reason` + `source` disambiguates context:

| Reason | Source | Example |
|--------|--------|---------|
| `NoRoute` | `ActorRuntime` | Local send to unknown actor |
| `NoRoute` | `Discovery` | Registrar lookup returned no result |
| `Timeout` | `ActorRuntime` | Ask timed out waiting for response |
| `Timeout` | `Rpc` | RPC channel retry exhausted |
| `Timeout` | `Discovery` | Gossip probe timed out |
| `MailboxFull` | `Mailbox` | Local admission rejected at capacity |
| `MailboxFull` | `Transport` | Remote endpoint backpressure frame |

### 5.5 `retryable` Semantics

| Reason | retryable | Condition |
|--------|-----------|-----------|
| `NoRoute` | true | Actor may be spawning |
| `NodeUnavailable` | true | Node may recover |
| `ActorDead` | false | Dead is terminal |
| `ActorNotReady` | true | Actor may become ready |
| `Quarantined` | false | Quarantine is intentional |
| `CircuitOpen` | true | May close after cooldown |
| `MailboxFull` | true | Drain happens |
| `OutboundQueueFull` | true | Drain happens |
| `MemoryPressure` | true | Transient |
| `Expired` | false | Time has passed |
| `Timeout` | true | May succeed with more time |
| `RejectedByPolicy` | false | Policy is intentional |
| `Dropped` | false | Drop is intentional |
| `MailboxClosed` | false | Actor is gone |
| `SerializationError` | false | Invalid data |
| `TransportError` | true | Network is transient |
| `FrameRejected` | false | Invalid frame |
| `Duplicate` | false | Already delivered |
| `Draining` | true | May complete soon |
| `ShuttingDown` | true | Try another node |
| `RetryExhausted` | false | No more attempts |
| `SpawnFailed` | false | Spawn is terminal |
| `Unknown` | false | Unknown, assume terminal |

### 5.6 Mapping From `EnqueueResultCode`

Existing callers of `try_push()` or `try_send()` receive `EnqueueResult`. The
mapping to `FailureReason` is:

| `EnqueueResultCode` | `FailureReason` | `retryable` |
|---------------------|-----------------|-------------|
| `Accepted` | _(not a failure)_ | — |
| `AcceptedWithSoftPressure` | _(not a failure)_ | — |
| `Rejected` | `RejectedByPolicy` | false |
| `DroppedNewest` | `Dropped` | false |
| `DroppedExisting` | `Dropped` | false |
| `ReroutedToDeadLetter` | `RejectedByPolicy` | false |
| `ReroutedToOverflow` | `RejectedByPolicy` | false |
| `MailboxClosed` | `MailboxClosed` | false |
| `ActorNotFound` | `NoRoute` | true |

`EnqueueResult` gains a `to_failure_envelope()` method that fills a
`FailureEnvelope` with the subset of fields the mailbox layer knows (reason,
actor_id, retryable, source=Mailbox). The remaining fields (sender, receiver,
message_id, trace) must be added by the caller that has the message context.

### 5.7 Mapping From `errors::` Codes

The existing `error` class uses `uint32_t` codes with no range discipline.
HTTP protocol error codes start at 2000. The mapping is:

| `errors::` constant | `FailureReason` |
|---------------------|-----------------|
| `unknown` (1) | `Unknown` |
| `actor_down` (2) | `ActorDead` |
| `actor_not_found` (3) | `NoRoute` |
| `mailbox_full` (4) | `MailboxFull` |
| `timeout` (5) | `Timeout` |
| `invalid_argument` (6) | `RejectedByPolicy` |
| `http_parse_error` (2001) | `SerializationError` |
| `http_connect_failed` (2002) | `TransportError` |
| `http_timeout` (2003) | `Timeout` |

The `error` class gains:
```cpp
class error {
  public:
    // Existing constructors preserved.

    /// Construct with a failure envelope for observability.
    error(FailureReason reason, std::string msg);

    /// Optional failure envelope. Returns nullptr if not set.
    const FailureEnvelope* envelope() const;

  private:
    uint32_t code_ = 0;
    std::string message_;
    // Optional failure envelope for observability correlation.
    std::unique_ptr<FailureEnvelope> envelope_;
};
```

### 5.8 Relationship to Planned `DeliveryStatus` and `DeadLetterReason`

This design supersedes the standalone `DeliveryStatus` and `DeadLetterReason`
enums from the delivery-semantics and DLQ design docs. Instead:

- The delivery result struct (future `DeliveryResult`) carries a `FailureReason`
  field, not a separate `DeliveryStatus` enum.
- The DLQ record struct carries a `FailureReason` field, not a separate
  `DeadLetterReason` enum.
- Both carry the full `FailureEnvelope` for observability.

The superseded enums' values map directly onto `FailureReason`:

| `DeliveryStatus` (planned) | `FailureReason` |
|---------------------------|-----------------|
| `Accepted` | _(not a failure)_ |
| `NoRoute` | `NoRoute` |
| `ActorDead` | `ActorDead` |
| `MailboxFull` | `MailboxFull` |
| `Expired` | `Expired` |
| `Duplicate` | `Duplicate` |
| `RemoteUnavailable` | `NodeUnavailable` |
| `RejectedByPolicy` | `RejectedByPolicy` |
| `SerializationError` | `SerializationError` |
| `TransportError` | `TransportError` |

| `DeadLetterReason` (planned) | `FailureReason` |
|------------------------------|-----------------|
| `NoRoute` | `NoRoute` |
| `ActorDead` | `ActorDead` |
| `MailboxFull` | `MailboxFull` |
| `Expired` | `Expired` |
| `SerializationError` | `SerializationError` |
| `TransportError` | `TransportError` |
| `RejectedByPolicy` | `RejectedByPolicy` |
| `NodeUnavailable` | `NodeUnavailable` |
| `Quarantined` | `Quarantined` |

### 5.9 `retryable()` Free Function

```cpp
constexpr bool retryable(FailureReason reason) noexcept {
    switch (reason) {
        case FailureReason::NoRoute:
        case FailureReason::NodeUnavailable:
        case FailureReason::ActorNotReady:
        case FailureReason::CircuitOpen:
        case FailureReason::MailboxFull:
        case FailureReason::OutboundQueueFull:
        case FailureReason::MemoryPressure:
        case FailureReason::Timeout:
        case FailureReason::TransportError:
        case FailureReason::Draining:
        case FailureReason::ShuttingDown:
            return true;
        default:
            return false;
    }
}
```

### 5.10 `to_string()` Free Function

```cpp
const char* to_string(FailureReason reason) noexcept;
// Returns snake_case string literal for metrics labels, log keys, and CLI.
```

### 5.11 Integration Points

#### 5.11.1 Actor Send Path

```
ActorContext::try_send(addr, msg)
    └── ActorSystem::try_deliver_local(addr, msg)
        ├── registry lookup → FailureReason::NoRoute
        ├── lifecycle check → FailureReason::ActorDead / ActorNotReady
        └── mailbox->try_push(msg)
            ├── Accepted → success
            └── rejected → map EnqueueResultCode → FailureReason
                            build FailureEnvelope with sender, receiver,
                            message_id, trace, retryable, timestamp, detail
                            → log, trace attribute, metric counter
```

`try_send()` signature:
```cpp
mailbox::EnqueueResult try_send(const ActorAddress& target, TypedMessage msg,
                                DeliveryOptions opts = {});
```
The `EnqueueResult` return type is preserved for source compatibility. The
`FailureEnvelope` is built internally and pushed to observability paths (log,
trace, metric). A future `try_send_envelope()` overload can expose the full
envelope.

#### 5.11.2 RPC Path

```
RpcChannel::call(target, payload, timeout)
    └── on timeout → FailureReason::Timeout
    └── on send failure → map EnqueueResultCode → FailureReason
    └── on remote error → parse error code → FailureReason
    └── on retry exhaust → FailureReason::RetryExhausted
```

RPC error responses gain a `FailureEnvelope` in their error payload so the
caller receives the full metadata.

#### 5.11.3 Spawn Path

```
SpawnReceiver::handle_spawn_request(req)
    └── type registry lookup → FailureReason::NoRoute
    └── authorization failure → FailureReason::RejectedByPolicy
    └── constructor failure → FailureReason::SpawnFailed
    └── build FailureEnvelope with source=Spawn, actor_id=target, detail=...
```

`AsyncActor` stores a `FailureEnvelope` alongside the spawn error code so
callers can inspect the full failure.

#### 5.11.4 Dead-Letter Queue

DLQ records replace `DeadLetterReason` with `FailureReason` from the
`FailureEnvelope`. The DLQ record struct adds a `FailureEnvelope` field
alongside the existing payload fields.

The DLQ design doc's record struct is updated to:
```cpp
struct DeadLetterRecord {
    FailureEnvelope failure;  // replaces standalone reason + timestamp fields
    TypedMessage message;     // retained payload (policy-dependent)
    uint32_t attempt{0};
    uint32_t payload_size{0};
};
```

#### 5.11.5 Tracing

Actor receive spans add span attributes from the failure envelope:
- `failure.reason` = snake_case reason string
- `failure.source` = subsystem source string
- `failure.retryable` = "true" / "false"
- `failure.detail` = truncated detail string

These are set on the span when a message handler produces a failure, or on the
send span when delivery fails.

#### 5.11.6 Metrics

A shared counter with `reason` and `source` labels:

```
hpactor_failures_total{reason="no_route",source="actor_runtime",retryable="true"}
```

Subsystem-specific counters (e.g., `hpactor_mailbox_admission_total`) continue
to exist. The shared counter provides a single query for "how many failures
happened, and why."

#### 5.11.7 CLI

```
/failure summary [--since 10m]
/failure list [--reason MailboxFull] [--limit 50]
/failure show <failure_id>
```

If a recent-failure ring buffer is added, CLI can query it. Otherwise the CLI
reads aggregated metrics and recent log events.

## 6. Observability

### 6.1 Metrics

| Metric | Labels | Purpose |
|--------|--------|---------|
| `hpactor_failures_total` | `reason`, `source`, `retryable` | All failure counts |
| `hpactor_mailbox_admission_total` | `code` (existing) | Continues, plus `failure_reason` label |

### 6.2 Logs

Every `FailureEnvelope` produces a structured log event:

```json
{
  "event": "failure",
  "reason": "mailbox_full",
  "source": "mailbox",
  "actor_id": 42,
  "sender": "ipv4://10.0.0.1:9000/actor/7",
  "receiver": "ipv4://10.0.0.1:9000/actor/42",
  "message_id": 18446744073709551615,
  "trace_id": "0af7651916cd43dd8448eb211c80319c",
  "span_id": "b7ad6b7169203331",
  "retryable": true,
  "detail": "mailbox depth=1024 capacity=1024 policy=reject_newest"
}
```

### 6.3 Traces

Failed delivery spans set attributes from the `FailureEnvelope`. Failed handler
execution spans (where the actor's handler returns an `error`) also set
attributes.

## 7. Source Layout

```
include/hpactor/types/
    failure_reason.hpp      // FailureReason enum, FailureSource enum,
                            // retryable(), to_string()
    failure_envelope.hpp    // FailureEnvelope struct
    types.hpp               // error class gains optional FailureEnvelope

include/hpactor/mailbox/
    mailbox_policy.hpp      // EnqueueResult gains to_failure_envelope()

include/hpactor/tracing/
    span.hpp                // Span gains set_failure(FailureEnvelope)

src/types/
    failure_reason.cpp      // to_string() implementation
```

`FailureReason` and `FailureEnvelope` are header-only types. The only compiled
translation unit is `failure_reason.cpp` for `to_string()` (avoids duplicate
string literals).

## 8. Implementation Plan

### Phase 1: Shared Types (this change)

1. Add `FailureReason` enum to `include/hpactor/types/failure_reason.hpp`.
2. Add `FailureSource` enum alongside it.
3. Add `retryable(FailureReason)` and `to_string(FailureReason)`.
4. Add `FailureEnvelope` struct to `include/hpactor/types/failure_envelope.hpp`.
5. Add `EnqueueResult::to_failure_envelope()` that fills reason + actor_id +
   retryable from the mailbox's perspective.
6. Add optional `FailureEnvelope` to the `error` class with constructor
   overload and accessor.
7. Add `failure_reason.cpp` in `src/types/` with the `to_string()` table.
8. Wire `try_send()` and `try_deliver_local()` to build `FailureEnvelope` on
   failure and emit log + metric.
9. Wire the receive-span path to set span attributes from `FailureEnvelope` when
   a handler returns an `error` with an envelope.

### Phase 2: DLQ Integration (follow-on)

10. Update `DeadLetterRecord` to use `FailureReason` and carry full
    `FailureEnvelope`.
11. Update the DLQ actor snapshot/pop/replay to expose `FailureReason`.

### Phase 3: RPC + Spawn Integration (follow-on)

12. Update `RpcChannel` error paths to build `FailureEnvelope`.
13. Update `SpawnReceiver` and `AsyncActor` to carry `FailureEnvelope`.

### Phase 4: CLI + Admin API (follow-on)

14. Add `/failure summary` and `/failure list` CLI commands.
15. Expose failure counters through Admin API.

## 9. Compatibility

### 9.1 Preserved

- `void send()` fire-and-forget API.
- `EnqueueResultCode` enum values and `EnqueueResult` fields.
- `try_send()` return type (`EnqueueResult`).
- `error` class public API (gains new constructor + accessor, no removals).
- `errors::` namespace constants.
- `try_push()` signature and return type.

### 9.2 Modified

- `error` gains internal `unique_ptr<FailureEnvelope>` member. The class is
  already move-only through `std::string`. Size increases by one pointer.
- `EnqueueResult` gains `to_failure_envelope()` method. No field changes.
- DLQ record struct conceptually replaces `DeadLetterReason` with
  `FailureReason` — this is not yet implemented so no breakage.

### 9.3 Superseded

- The planned `DeliveryStatus` enum from `actor-delivery-semantics-design.md`
  is superseded by `FailureReason`. The delivery semantics design doc should
  reference `FailureReason` instead of defining its own enum.
- The planned `DeadLetterReason` enum from `dead-letter-queue-design.md` is
  superseded by `FailureReason`. The DLQ design doc should reference
  `FailureReason` instead of defining its own enum.

## 10. Acceptance Criteria

- [ ] `FailureReason` enum covers every failure path: send, ask, RPC, spawn,
  DLQ, transport, policy, time, lifecycle.
- [ ] `FailureEnvelope` includes actor id, sender, receiver, message id, trace
  id, reason, retryable flag, timestamp, source, and detail.
- [ ] Every failure code path in `try_send()` and `try_deliver_local()`
  produces a `FailureEnvelope`.
- [ ] `EnqueueResultCode` values map to correct `FailureReason` values.
- [ ] `errors::` codes map to correct `FailureReason` values.
- [ ] `retryable(FailureReason)` returns the correct flag for every reason.
- [ ] Span attributes include `failure.reason` and `failure.source`.
- [ ] `hpactor_failures_total{reason,source,retryable}` metric is emitted.
- [ ] Existing `send()`, `try_send()`, `try_push()` APIs are source-compatible.
- [ ] No new production failure path returns `FailureReason::Unknown` — every
  path produces a specific reason.

## 11. Test Plan

### 11.1 Unit Tests

- `FailureReason` ↔ `EnqueueResultCode` mapping correctness.
- `FailureReason` ↔ `errors::` code mapping correctness.
- `retryable()` correctness for every `FailureReason` value.
- `to_string()` round-trip coverage.
- `EnqueueResult::to_failure_envelope()` fills correct fields.
- `error` class with and without `FailureEnvelope`.
- `FailureEnvelope` move semantics.

### 11.2 Integration Tests

- `try_send()` to dead actor → `FailureReason::ActorDead`, envelope fields.
- `try_send()` to full mailbox → `FailureReason::MailboxFull`, envelope fields.
- `try_send()` to unknown actor → `FailureReason::NoRoute`, envelope fields.
- Trace span has failure attributes when handler returns error with envelope.
- Metric counter increments for each failure path.

### 11.3 DLQ Integration Tests (Phase 2)

- DLQ record captures `FailureReason` and full envelope fields.
- DLQ snapshot/pop/replay preserves `FailureReason`.

### 11.4 RPC + Spawn Tests (Phase 3)

- RPC timeout produces `FailureEnvelope` with `FailureReason::Timeout`.
- RPC retry exhaustion produces `FailureReason::RetryExhausted`.
- Spawn failure produces `FailureEnvelope` with `FailureReason::SpawnFailed`.
