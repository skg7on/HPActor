# ACT-006: Actor-Local Rate Limiting and Admission Policy — Design Spec

**Issue**: [#11](https://github.com/skg7on/HPActor/issues/11)
**Subsystem**: Actor Runtime
**Priority**: P1
**Release lane**: Foundation
**Backlog source**: `docs/architecture/production/architecture-requirement-backlog.md#L30`
**Architecture doc**: `docs/architecture/production/production-reliability-plane.md`

## 1. Executive Summary

HPActor's mailbox already enforces a hard capacity limit per actor with configurable overflow policies (reject, drop, dead-letter, spill), multi-lane priority routing, watermarked pressure states, and backpressure signal propagation. Producers can receive `EnqueueResult` from `try_send()` to learn that an actor is overloaded.

But the current admission model is purely depth-based: a message is admitted unless the mailbox is full. There is no way to:

- **Rate-limit** an actor's processing speed so it cannot consume more than N messages per second, even when mailbox depth is shallow. Without this, a fast producer can keep a slow actor perpetually busy while other actors starve for scheduler time.
- **Selectively admit** messages based on message type, sender, priority, or payload characteristics — all messages are treated equally regardless of importance or trust.
- **Throttle per-sender or per-message-type** to isolate a noisy or hostile producer from flooding an actor with legitimate-looking messages.

ACT-006 introduces two complementary mechanisms:

1. **Token-bucket rate limiting on the consumption (dequeue) side** — an actor can be configured to process at most `N` messages per second, with configurable burst allowance. Excess consumption tokens accumulate queueing delay rather than rejecting messages; the mailbox remains bounded and overflow policies still protect against depth exhaustion. The rate limiter acts as a gate between the mailbox and the actor runner.

2. **Policy-based admission on the enqueue side** — a configurable policy chain at the mailbox entry point evaluates each incoming message before capacity reservation. Policies can match on `TypeTag`, sender `ActorId`, message priority, payload size, or arbitrary custom predicates, and decide to `Accept`, `Reject` (with `FailureReason::RejectedByPolicy`), or `RerouteToDLQ`.

Together these give operators fine-grained control over actor message processing without changing actor code.

## 2. What Already Exists

### 2.1 Mailbox Admission (MBX-001, MBX-005 ✅)

| Item | Location | Notes |
|------|----------|-------|
| `MailboxConfig` | `mailbox/mailbox_policy.hpp` | `capacity`, `priority_levels`, `overflow_policy`, watermarks, signal interval |
| `ReservationManager` | `mailbox/detail/reservation_manager.hpp` | Atomic slot + byte reservation |
| `PressureStateMachine` | `mailbox/detail/pressure_state_machine.hpp` | Normal/Soft/Hard/Recovering |
| `BackpressureSignalGate` | `mailbox/detail/backpressure_signal_gate.hpp` | CAS-based signal rate limiter with escalation |
| `MPSCActorMailbox::try_push()` | `mailbox/mpsc_actor_mailbox.hpp` | Enqueue path with overflow policy dispatch |
| `MPSCActorMailbox::dequeue()` | `mailbox/mpsc_actor_mailbox.hpp` | Dequeue path with consumer lock + overflow drain |
| Overflow handlers | `mailbox/detail/handlers/*.hpp` | Reject, DropNewest, DropOldest, DropLowestPriority, DeadLetter, Spill, SignalOnly |
| `DeliveryOptions` | `mailbox/mailbox_policy.hpp` | Per-message delivery options (no_drop, delivery_mode, message_id) |

### 2.2 Actor Execution (scheduler)

| Item | Location | Notes |
|------|----------|-------|
| `BehaviorActorRunner::run()` | `sched/actor_execution_engine.cpp` | Dequeues one message per activation, calls `actor.receive()` |
| `ActorExecutionEngine` | `sched/actor_execution_engine.hpp` | Dispatches to Behavior or Coroutine runner |
| `ActorReadyGate` | `sched/actor_ready_gate.hpp` | CAS-based readiness admission for scheduler work stealing |

### 2.3 Existing rate-limiting primitives

| Item | Location | Notes |
|------|----------|-------|
| `BackpressureSignalGate::try_acquire()` | `mailbox/detail/backpressure_signal_gate.hpp` | Interval + severity-escalation rate limiter for backpressure signals, not for message processing |
| `FailureReason::RejectedByPolicy` (40) | `types/failure_reason.hpp` | Declared but currently unused; reserved for policy-based rejection |

### 2.4 Configuration & observability patterns

The project has established patterns for TOML subsystem parsers (self-registering via `TomlSystemParserRegistration`), CLI introspection via `/commands`, and metrics via `MetricEventType` enums and `MpscRingBuffer` emission. These are reused without architectural change.

## 3. Design

### 3.1 Overview

```
                         ┌─────────────────────────────────────────────┐
                         │             Actor Execution                  │
                         │  ┌──────────┐   ┌───────────┐               │
                         │  │  Rate     │   │  Behavior  │               │
     try_send()          │  │  Limiter  │   │  Runner    │               │
         │               │  │ (token    │◄──│  run()     │               │
     ┌───▼────────┐      │  │  bucket)  │   └───────────┘               │
     │  Admission  │      │  └──────────┘                               │
     │  Policy     │──►───┤                                             │
     │  Chain      │      │  ┌──────────────────────────────┐           │
     └───▲────────┘      │  │      MultiLaneQueue           │           │
         │               │  │  ┌──────────┬────┬────┬────┐  │           │
         │               │  │  │ System   │ P0 │ P1 │ P2 │  │           │
         │               │  │  └──────────┴────┴────┴────┘  │           │
         │               │  └──────────────────────────────┘           │
     ┌───┴──────────┐    └─────────────────────────────────────────────┘
     │  Producer    │
     │  (local or   │
     │   remote)    │
     └──────────────┘
```

Two new components are inserted into the actor hot path:

- **Admission Policy Chain** — called at the start of `MPSCActorMailbox::try_push()`, before reservation and overflow handling. Can reject or reroute messages early without consuming a reservation slot.
- **Rate Limiter** — called at the start of `MPSCActorMailbox::dequeue()`, before the lane dequeue. Controls whether the actor is allowed to consume another message right now.

Both are optional and configured per-actor with system defaults. Zero-cost when disabled: the rate limiter is a null check, the admission chain is an empty vector.

### 3.2 Token-Bucket Rate Limiter

#### Algorithm

Use the generic token bucket (r,t,b) model:

- **r** (rate): sustained message processing rate in messages/second.
- **b** (burst): maximum accumulated tokens; bounds the worst-case drain from the mailbox when a burst arrives after an idle period.
- **t** (interval between refills): 1 millisecond granularity refill timer, driven by steady_clock.

```cpp
class TokenBucketRateLimiter {
public:
    TokenBucketRateLimiter(double rate_per_sec, uint32_t burst) noexcept;

    /// Try to consume one token for message processing.
    /// Returns true if a token was available, false if rate limited.
    /// Updates last_refill_ns_ and tokens_ on refill.
    bool try_consume(uint64_t now_ns) noexcept;

    /// Peek at current token count (for CLI / metrics).
    double current_tokens() const noexcept;

    /// Reconfigure at runtime (TOML reload or admin API).
    void reconfigure(double rate_per_sec, uint32_t burst) noexcept;

private:
    std::atomic<int64_t> tokens_ns_{0};  // tokens stored as ns-resolution refill credit
    std::atomic<uint64_t> last_refill_ns_{0};
    double rate_per_ns_;                 // derived from configured rate
    uint32_t burst_;                     // max accumulated tokens
    double max_tokens_;                  // burst_ as double
};
```

**Refill logic** (called on every `try_consume`):

```
elapsed = now_ns - last_refill_ns_
if elapsed > 0:
    add = elapsed * rate_per_ns_   // tokens earned during elapsed
    tokens_ = min(tokens_ + add, max_tokens_)
    last_refill_ns_ = now_ns
```

**Consumption**:

```
if tokens_ >= 1.0:
    tokens_ -= 1.0
    return true (admitted)
else:
    return false (rate limited)
```

**Why token bucket instead of a simpler approach like fixed-window counter:**

Token buckets are already familiar in the codebase (the existing `BackpressureSignalGate` uses a similar interval+escalation CAS pattern). They handle bursts naturally without needing a separate burst-limit parameter to interact poorly with window edges, and the token math maps directly to `msg_per_sec` user intent.

#### Thread safety

The rate limiter lives inside `MPSCActorMailbox`, which already has well-defined threading: producers call `try_push` from any thread (lock-free MPSC), and the consumer calls `dequeue` from the single scheduler thread assigned to this actor. The `dequeue` path is already serialized by `consumer_lock_`. Therefore:

- `try_consume()` is called from the consumer thread (under `consumer_lock_`) — no atomic needed for the fast path, but the token and refill fields are kept atomic for safe CLI/metric reads from admin threads.
- `reconfigure()` may be called from any thread — atomic store is sufficient.

#### Behavior when rate-limited

When `try_consume()` returns `false`, the dequeue returns `nullptr` (no message delivered to the actor). The caller (BehaviorActorRunner) sees an empty result and checks whether the mailbox is still non-empty:

- If mailbox is non-empty → the actor is requeued with a short delay (via the scheduler's existing `schedule` mechanism or by the scheduler yielding and polling).
- If mailbox is empty → the actor transitions to idle.

The key insight is that rate limiting does NOT discard messages; it merely defers consumption. The mailbox's existing capacity limits and overflow policies still protect against unbounded growth. A heavily rate-limited actor may eventually hit capacity and activate overflow policies, which is correct behavior — the operator has chosen to limit processing speed, and if producers overwhelm that speed they get backpressure.

To avoid tight polling loops, the rate limiter can optionally signal a wakeup delay: when `try_consume()` returns false, it computes `time_until_next_token_ns` and the scheduler can use that to schedule a precise wakeup instead of busy-looping.

#### EnqueueResultCode additions

```
EnqueueResultCode::RateLimited    // message was not dequeued due to rate limit
```

### 3.3 Admission Policy Chain

#### Policy interface

```cpp
enum class AdmissionDecision : uint8_t {
    Accept,         // message admitted (default)
    Reject,         // rejected with FailureReason::RejectedByPolicy
    RerouteToDLQ,   // rerouted to dead-letter queue
};

struct AdmissionPolicyResult {
    AdmissionDecision decision{AdmissionDecision::Accept};
    FailureReason reason{FailureReason::RejectedByPolicy};
    const char* policy_name{nullptr};  // for metrics / CLI
    uint32_t match_code{0};            // opaque policy-specific code
};

class IAdmissionPolicy {
public:
    virtual ~IAdmissionPolicy() = default;
    virtual AdmissionPolicyResult evaluate(const TypedMessage& msg,
                                           const MailboxEnvelopeMeta& meta,
                                           const MailboxConfig& config,
                                           uint64_t mailbox_depth) noexcept = 0;
    virtual const char* name() const noexcept = 0;
};
```

#### Built-in policies

| Policy | Match Criteria | Decision |
|--------|---------------|----------|
| `TypeFilterPolicy` | Message TypeTag in allowed/blocked set | Reject if blocked, Accept otherwise |
| `SenderFilterPolicy` | Sender ActorId in allowed/blocked set | Reject if blocked, Accept otherwise |
| `PriorityThresholdPolicy` | Message priority below configured minimum | Reject, optionally RerouteToDLQ |
| `SizeLimitPolicy` | Payload size exceeds configured maximum | Reject or RerouteToDLQ |
| `PerSenderRatePolicy` | Per-sender token bucket (see 3.4) | Reject when per-sender rate exceeded |

#### Policy evaluation order

The policy chain is an ordered vector of `IAdmissionPolicy*` pointers. Each policy is evaluated in order. The first policy that returns `Reject` or `RerouteToDLQ` short-circuits the chain (fail-fast). If all policies return `Accept`, the message proceeds to normal mailbox admission.

#### Integration into try_push

The admission policy chain is evaluated at the top of `MPSCActorMailbox::try_push()`, *before* the system-lane capacity check:

```cpp
EnqueueResult try_push(T&& msg, const MailboxEnvelopeMeta& meta) noexcept {
    // ── Admission policy gate ──────────────────────────────
    if (admission_policies_ && !admission_policies_->empty()) [[unlikely]] {
        auto result = evaluate_admission_policies(msg, meta);
        if (result.decision != AdmissionDecision::Accept) {
            // Build rejection or DLQ reroute result
            total_rejected_by_policy_.fetch_add(1, std::memory_order_relaxed);
            // ... emit metrics ...
            EnqueueResult r;
            r.code = (result.decision == AdmissionDecision::RerouteToDLQ)
                         ? EnqueueResultCode::ReroutedToDeadLetter
                         : EnqueueResultCode::Rejected;
            r.failure_reason = result.reason;
            return r;
        }
    }
    // ── End admission policy gate ──────────────────────────

    // ... existing system lane check, reservation, overflow ...

    // ... existing enqueue path ...
}
```

#### EnqueueResultCode additions

No new codes needed for the admission chain — `Rejected` and `ReroutedToDeadLetter` already exist. The `FailureReason::RejectedByPolicy` (already declared) is set on the result when a policy rejects.

### 3.4 Per-Sender Rate Policy

`PerSenderRatePolicy` is a composite admission policy that maintains an independent token bucket per sender `ActorId`. This prevents one fast or faulty producer from crowding out other senders to a shared actor.

```cpp
class PerSenderRatePolicy : public IAdmissionPolicy {
public:
    PerSenderRatePolicy(double rate_per_sec, uint32_t burst,
                        uint32_t max_senders = 256) noexcept;

    AdmissionPolicyResult evaluate(const TypedMessage& msg,
                                   const MailboxEnvelopeMeta& meta,
                                   const MailboxConfig& config,
                                   uint64_t mailbox_depth) noexcept override;

    const char* name() const noexcept override { return "per_sender_rate"; }

private:
    struct alignas(64) SenderBucket {
        std::atomic<int64_t> tokens_ns;
        std::atomic<uint64_t> last_refill_ns;
    };

    double rate_per_ns_;
    uint32_t burst_;
    uint32_t max_senders_;
    // Lock-free for reads from consumer path, protected by mutex for
    // eviction and reconfigure.
    tbb::concurrent_hash_map<ActorId, std::unique_ptr<SenderBucket>> buckets_;
};
```

**Bucket eviction**: stale sender entries (no activity for > 2x refill period) are lazily evicted during evaluate or via a periodic maintenance call.

### 3.5 Rate Limiter — Full API

```cpp
/// \brief Token-bucket rate limiter for actor-local processing rate control.
///
/// Not thread-safe for the fast path (intended to be called under the
/// mailbox's consumer lock). Fields are atomic only for safe reads from
/// CLI / metrics / admin threads.
class ActorRateLimiter {
public:
    ActorRateLimiter() noexcept = default;

    /// Configure the rate limiter. When rate_per_sec is <= 0, the
    /// limiter is disabled (try_consume always returns true).
    void configure(double rate_per_sec, uint32_t burst) noexcept;

    /// Try to consume one token for processing a message.
    /// Returns true if a token was available or rate limiting is disabled.
    /// Must be called from the consumer thread.
    bool try_consume(uint64_t now_ns) noexcept;

    /// Time until the next token is available, in nanoseconds.
    /// Returns UINT64_MAX if unlimited tokens are available.
    /// Returns 0 if a token is available now.
    uint64_t time_until_next_token_ns(uint64_t now_ns) const noexcept;

    /// Current token count (for CLI / metrics snapshot).
    double current_tokens() const noexcept;

    /// Configured rate (for CLI / metrics).
    double configured_rate() const noexcept;
    uint32_t configured_burst() const noexcept;

    /// Whether rate limiting is enabled.
    bool is_enabled() const noexcept;

private:
    // Implementation detail: tokens stored as a double with atomic
    // operations for safe reads. The consumer thread does a relaxed
    // load + conditional store, which is correct because no other
    // thread writes tokens_.
    std::atomic<double> tokens_{0.0};
    std::atomic<uint64_t> last_refill_ns_{0};
    double rate_per_ns_{0.0};
    uint32_t burst_{0};
    double max_tokens_{0.0};
    bool enabled_{false};
};
```

### 3.6 Integration into MPSCActorMailbox

`MPSCActorMailbox` gains two new members and a new `set_rate_limiter` / `set_admission_policies` API:

```cpp
template <typename T>
class MPSCActorMailbox {
    // ... existing members ...

    // New optional members (default: nullptr / disabled)
    std::unique_ptr<ActorRateLimiter> rate_limiter_;
    std::shared_ptr<std::vector<std::unique_ptr<IAdmissionPolicy>>>
        admission_policies_;

public:
    void set_rate_limiter(std::unique_ptr<ActorRateLimiter> limiter) noexcept;
    void set_admission_policies(
        std::shared_ptr<std::vector<std::unique_ptr<IAdmissionPolicy>>> policies) noexcept;
    const ActorRateLimiter* rate_limiter() const noexcept;
    const std::vector<std::unique_ptr<IAdmissionPolicy>>* admission_policies() const noexcept;

    // ... existing public API unchanged ...
};
```

**Dequeue changes**:

```cpp
T* dequeue() noexcept {
    lock_consumer();

    // ── Rate limiter gate ──────────────────────────────────
    if (rate_limiter_ && rate_limiter_->is_enabled()) [[unlikely]] {
        uint64_t now_ns = steady_now_ns();
        if (!rate_limiter_->try_consume(now_ns)) {
            // Rate limited — return nullptr without dequeuing.
            // The caller (BehaviorActorRunner) will see an empty
            // result and requeue the actor.
            unlock_consumer();
            return nullptr;
        }
    }
    // ── End rate limiter gate ──────────────────────────────

    // ... existing dequeue logic (lanes_.dequeue(), reservation release, etc.) ...
}
```

**Snapshot changes**: `MPSCActorMailbox::snapshot()` is extended to include rate limiter state (`is_enabled`, `current_tokens`, `configured_rate`, `configured_burst`, `admission_policies_count`).

### 3.7 Integration into BehaviorActorRunner

The `BehaviorActorRunner::run()` method already handles the case where `try_pop()` returns `false` (no message in mailbox). When the rate limiter gates dequeue, `try_pop` returns false while the mailbox is non-empty. The runner already handles this correctly:

```cpp
if (mailbox->try_pop(msg)) {
    // ... process message ...
}

if (!mailbox->empty()) {
    // Requeue actor — this path is taken both when there are
    // messages that couldn't be dequeued due to rate limiting
    // AND when there are genuinely more messages to process.
    auto admission = ready_gate_.mark_ready_already_admitted(actor);
    if (admission.accepted() ||
        admission.code == ReadyAdmissionCode::AlreadyReady) {
        return {ActorRunDisposition::RequeueReady, 0, INT64_MAX};
    }
}
```

However, to avoid tight polling when rate limited, the runner should also check if the rate limiter has a predictable next-token time and schedule a wakeup:

```cpp
if (mailbox->try_pop(msg)) {
    // ... process message ...
}

// ── Rate limiter wakeup scheduling ──────────────────────
if (!mailbox->empty()) {
    if (auto* limiter = mailbox->rate_limiter();
        limiter && limiter->is_enabled()) [[unlikely]] {
        uint64_t delay_ns = limiter->time_until_next_token_ns(steady_now_ns());
        if (delay_ns > 0 && delay_ns < UINT64_MAX) {
            // Schedule a timer wakeup instead of busy-looping.
            // The scheduler will re-activate this actor after delay_ns.
            auto delay = std::chrono::nanoseconds(delay_ns);
            actor.schedule(delay, TypedMessage::create_wakeup());
        }
    }
    // ... existing requeue logic ...
}
// ── End rate limiter wakeup scheduling ──────────────────
```

This requires `EventBasedActor` to support a lightweight wakeup message, which can be a new system `TypeTag::WakeupTag` that the behavior handler treats as a no-op (the real message is still in the mailbox and will be delivered on the next dequeue).

**Simplification for v1**: Skip the precise wakeup scheduling. Dequeue returns nullptr, runner sees non-empty mailbox, re-queues actor normally. On systems with many actors, the scheduler's existing work-stealing loop provides enough interleaving. The wakeup timer optimization can be added in a follow-up if measurement shows excessive polling.

### 3.8 Configuration

#### TOML format

Per-actor rate limiting and admission policy is configured through `[system.rate_limiting]` and `[admission]` sections, with per-actor overrides in the actor definition:

```toml
[system.rate_limiting]
enabled = false                    # global disable (default)
default_rate = 100.0               # messages/second (only when enabled)
default_burst = 10                 # burst tokens

[system.admission]
enabled = false                    # global disable (default)

[system.admission.rules]
# Block certain message types globally
type_blocked = ["DebugMessageTag", "HealthCheckTag"]

# Per-sender rate limits
[[system.admission.per_sender]]
rate = 50.0
burst = 5
max_senders = 256

[[actor]]
name = "my_actor"
behavior = "order_handler"

[actor.rate_limiting]
enabled = true
rate = 500.0                       # messages/second
burst = 50

[actor.admission]
enabled = true

[actor.admission.rules]
type_allowed = ["OrderRequestTag", "PaymentResultTag", "InventoryUpdateTag"]
priority_min = 2
size_max_bytes = 65536

# Per-sender override
[[actor.admission.per_sender]]
rate = 200.0
burst = 20
```

#### TOML parser

A new self-registering subsystem parser `src/config/parsers/rate_limiting_config_parser.cpp` is added, following the established `TomlSystemParserRegistration<T>` pattern. It parses the `[system.rate_limiting]` and `[system.admission]` sections and stores parsed policy specs in a `RateLimitingConfig` structure that `BootstrapEngine` passes to spawned actors.

```cpp
struct PerActorRateLimitSpec {
    bool enabled{false};
    double rate_per_sec{0.0};
    uint32_t burst{0};
};

struct AdmissionRuleSpec {
    bool enabled{false};
    std::vector<uint32_t> type_allowed_tags;
    std::vector<uint32_t> type_blocked_tags;
    std::vector<uint64_t> sender_blocked_ids;
    uint32_t priority_min{0};
    uint64_t size_max_bytes{0};
    // per-sender rate limits
    double per_sender_rate{0.0};
    uint32_t per_sender_burst{0};
    uint32_t max_senders{256};
};

struct RateLimitingConfig {
    PerActorRateLimitSpec system_default;
    AdmissionRuleSpec system_admission;
    // Per-actor overrides are stored keyed by actor name in
    // TopologyModel and merged during bootstrap.
};
```

### 3.9 Metrics

New `MetricEventType` values:

```
kRateLimitBlocked     // message deferred due to rate limit (counter)
kAdmissionRejected    // message rejected by admission policy (counter)
kAdmissionDLQRouted   // message rerouted to DLQ by admission policy (counter)
kPerSenderBucketCount // number of active per-sender buckets (gauge)
```

New metric event emission points:

- `MPSCActorMailbox::dequeue()` when rate limited → `kRateLimitBlocked`
- `MPSCActorMailbox::try_push()` when policy rejects → `kAdmissionRejected`
- `MPSCActorMailbox::try_push()` when policy routes to DLQ → `kAdmissionDLQRouted`

Metric names (OpenMetrics format):

```
# HELP hpactor_actor_rate_limit_total Number of messages rate-limited (deferred) by token bucket
# TYPE hpactor_actor_rate_limit_total counter
hpactor_actor_rate_limit_total{actor_id="12345"} 42

# HELP hpactor_actor_admission_rejected_total Number of messages rejected by admission policy
# TYPE hpactor_actor_admission_rejected_total counter
hpactor_actor_admission_rejected_total{actor_id="12345",policy="type_filter"} 7

# HELP hpactor_actor_admission_dlq_routed_total Number of messages rerouted to DLQ by admission policy
# TYPE hpactor_actor_admission_dlq_routed_total counter
hpactor_actor_admission_dlq_routed_total{actor_id="12345",policy="size_limit"} 3

# HELP hpactor_actor_rate_limiter_tokens Current token bucket balance
# TYPE hpactor_actor_rate_limiter_tokens gauge
hpactor_actor_rate_limiter_tokens{actor_id="12345"} 8.5

# HELP hpactor_actor_per_sender_buckets Number of active per-sender rate limit buckets
# TYPE hpactor_actor_per_sender_buckets gauge
hpactor_actor_per_sender_buckets{actor_id="12345"} 12
```

### 3.10 CLI

New CLI commands:

```
/actor <id> rate           — Show rate limiter state (enabled, rate, burst, tokens, blocked_total)
/actor <id> admission      — Show admission policy chain (policies, reject count per policy)
/actor <id> admission per-sender — List per-sender buckets and their current token counts
```

Existing commands extended:

```
/actor <id>                — Existing inspect; add rate limiter status and blocked count
```

CLI output format follows established `pretty_formatter` / `tabular_formatter` conventions.

### 3.11 Snapshot Extensions

`cli::MboxSnapshot` gains:

```cpp
// Rate limiter fields
bool rate_limiter_enabled{false};
double rate_limiter_rate{0.0};
uint32_t rate_limiter_burst{0};
double rate_limiter_current_tokens{0.0};
uint64_t rate_limit_blocked_total{0};

// Admission policy fields
uint8_t admission_policy_count{0};
uint64_t admission_rejected_total{0};
uint64_t admission_dlq_routed_total{0};
```

## 4. Implementation Plan

### Phase 1: Core Token Bucket (estimated 3-4 days)

| Step | Files | Description |
|------|-------|-------------|
| 1.1 | `include/hpactor/mailbox/actor_rate_limiter.hpp` | Token bucket implementation with `try_consume()`, `time_until_next_token_ns()`, `configure()` |
| 1.2 | `test/unit/mailbox/test_actor_rate_limiter.cpp` | Unit tests for token bucket: steady rate, burst, idle refill, edge cases (0 rate, max rate, overflow) |
| 1.3 | `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` | Add `ActorRateLimiter` member, `set_rate_limiter()`, rate-limit gate in `dequeue()`, snapshot extensions |
| 1.4 | `test/unit/mailbox/test_rate_limiter_mailbox.cpp` | Unit tests: rate-limited dequeue returns nullptr, snapshot reflects limiter state, counter increments |
| 1.5 | `include/hpactor/sched/actor_execution_engine.hpp` | Integrate rate-limiter-aware wakeup in `BehaviorActorRunner::run()` |

### Phase 2: Admission Policy Chain (estimated 3-4 days)

| Step | Files | Description |
|------|-------|-------------|
| 2.1 | `include/hpactor/mailbox/admission_policy.hpp` | `IAdmissionPolicy` interface, `AdmissionDecision`, `AdmissionPolicyResult` |
| 2.2 | `include/hpactor/mailbox/detail/policies/*.hpp` | Built-in policies: `TypeFilterPolicy`, `SenderFilterPolicy`, `PriorityThresholdPolicy`, `SizeLimitPolicy`, `PerSenderRatePolicy` |
| 2.3 | `test/unit/mailbox/test_admission_policies.cpp` | Unit tests for each built-in policy |
| 2.4 | `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` | Add admission policy vector, gate in `try_push()`, policy-related counters in snapshot |
| 2.5 | `test/unit/mailbox/test_admission_mailbox.cpp` | Unit tests: policy chain evaluation, short-circuit, rejection path, DLQ reroute path |

### Phase 3: Metrics & CLI (estimated 2 days)

| Step | Files | Description |
|------|-------|-------------|
| 3.1 | `include/hpactor/metrics/metrics_event.hpp` | Add `kRateLimitBlocked`, `kAdmissionRejected`, `kAdmissionDLQRouted`, `kPerSenderBucketCount` |
| 3.2 | `src/cli/commands/actor_commands.cpp` | Add `/actor <id> rate` and `/actor <id> admission` commands |
| 3.3 | `test/unit/cli/test_actor_commands.cpp` | CLI command tests |

### Phase 4: TOML Configuration (estimated 2-3 days)

| Step | Files | Description |
|------|-------|-------------|
| 4.1 | `include/hpactor/config/rate_limiting_config.hpp` | `RateLimitingConfig`, `PerActorRateLimitSpec`, `AdmissionRuleSpec` structs |
| 4.2 | `src/config/parsers/rate_limiting_config_parser.cpp` | Self-registering TOML subsystem parser using `TomlSystemParserRegistration` |
| 4.3 | `include/hpactor/config/topology_model.hpp` | Extend `ActorDef` with rate limiting and admission spec fields |
| 4.4 | `src/config/bootstrap_engine.cpp` | Wire rate limiter and admission policy construction during actor spawn |
| 4.5 | `test/integration/config/test_rate_limiting_config.cpp` | Integration tests for TOML parsing |

### Phase 5: Integration & System Tests (estimated 2-3 days)

| Step | Files | Description |
|------|-------|-------------|
| 5.1 | `test/integration/actor/test_rate_limiting.cpp` | Full integration: actor with rate limit, producer sends fast, verify consumption rate |
| 5.2 | `test/integration/actor/test_admission_policy.cpp` | Full integration: admission policy rejects by type, by sender, by size |
| 5.3 | `test/system/test_rate_limiting_topology.cpp` | System test: TOML-configured rate limiting and admission |
| 5.4 | `test/system/test_per_sender_isolation.cpp` | System test: one fast sender does not crowd out other senders |

### Phase 6 (follow-up, P2): Precise Wakeup Optimization

| Step | Files | Description |
|------|-------|-------------|
| 6.1 | `include/hpactor/types/types.hpp` | Add `TypeTag::WakeupTag` (value: 34) and `TypedMessage::create_wakeup()` |
| 6.2 | `include/hpactor/actor/event_based_actor.hpp` | Handle `WakeupTag` in default behavior (no-op) |
| 6.3 | `src/sched/actor_execution_engine.cpp` | Schedule wakeup timer when rate limiter reports delay |

## 5. Non-Goals

- **Distributed rate limiting** (coordination across nodes). This design is actor-local only. Cross-node rate limiting would require a distributed token-bucket protocol and is deferred to a separate cluster-plane feature.
- **Rate limiting for coroutine actors in v1.** Coroutines suspend at `co_await` boundaries; the rate limiter integration for coroutines needs different wiring (rate-limit the `mailbox_awaiter` rather than `dequeue`). v1 covers behavior (event-based) actors only.
- **Dynamic policy reload** (re-reading TOML at runtime without restart). The TOML config is loaded at startup. A follow-up ACT feature for dynamic config reload is tracked separately.
- **Priority-weighted rate limiting** (favoring high-priority messages during rate-limited intervals). The existing priority-aware multi-lane dequeue already delivers high-priority messages first; rate limiting preserves that ordering within the token budget.
- **Weighted fair queuing** or other sophisticated scheduler-side rate control. The token bucket / admission policy approach is deliberately simple and composable with the existing mailbox architecture.

## 6. Open Questions

1. **Rate limit unit**: should the token bucket count messages or bytes? Messages is simpler and maps to the existing capacity model. A separate byte-based rate limiter could be added as a second policy or limiter variant later.
2. **Per-sender bucket eviction**: what is the eviction policy for stale `PerSenderRatePolicy` buckets? Recommend: LRU with max_senders bound, lazy eviction during `evaluate()`, and a background maintenance timer for cleanup.
3. **Backpressure interaction**: when the rate limiter is actively blocking and the mailbox stays full, should the backpressure signal gate trigger? Yes — the rate limiter reduces consumption, which keeps depth high, which is the correct condition for upstream backpressure signaling. No special handling needed.
4. **Rate limit zero**: when `rate_per_sec = 0`, the token bucket never refills, so `try_consume()` returns false after the initial burst is exhausted. This effectively pauses the actor. Useful for debugging or maintenance but should be documented clearly.

## 7. Evidence: Acceptance Criteria

### Rate Limiting

- [ ] Token bucket refills at the configured rate over a 5-second observation window (±2% tolerance).
- [ ] Burst allows a short spike above the sustained rate.
- [ ] After an idle period, the bucket refills to `burst` tokens.
- [ ] When rate limited, `dequeue()` returns `nullptr` while mailbox is non-empty.
- [ ] `MboxSnapshot` reports rate limiter state correctly.
- [ ] Metric `hpactor_actor_rate_limit_total` increments on each rate-limited dequeue.
- [ ] CLI `/actor <id> rate` shows enabled/disabled, rate, burst, tokens, blocked count.

### Admission Policy

- [ ] `TypeFilterPolicy` blocks messages with a specified TypeTag.
- [ ] `SenderFilterPolicy` blocks messages from a specified sender.
- [ ] `PriorityThresholdPolicy` rejects messages below minimum priority.
- [ ] `SizeLimitPolicy` rejects messages exceeding size threshold.
- [ ] Policy chain short-circuits on first rejection.
- [ ] `PerSenderRatePolicy` independently rate-limits each sender.
- [ ] `EnqueueResult` contains `FailureReason::RejectedByPolicy` when rejected.
- [ ] Metric `hpactor_actor_admission_rejected_total` increments on each rejection.
- [ ] CLI `/actor <id> admission` shows active policies and per-policy rejection counts.

### TOML Configuration

- [ ] `[system.rate_limiting]` sets system defaults.
- [ ] `[actor.rate_limiting]` overrides per actor.
- [ ] `[system.admission.*]` configures system-wide admission rules.
- [ ] `[actor.admission.*]` configures per-actor admission rules.
- [ ] Per-actor config wins over system defaults.

### Integration

- [ ] Fast producer + rate-limited actor: actual consumption rate matches configured rate.
- [ ] Admission policy rejects specific message types: confirmed via `EnqueueResult`.
- [ ] Per-sender isolation: one fast sender's rate limit does not affect other senders.
- [ ] Rate limiting + backpressure: high mailbox depth still triggers backpressure signals.
- [ ] Rate limiting + overflow: rate-limited actor whose mailbox fills up correctly activates overflow handlers.

