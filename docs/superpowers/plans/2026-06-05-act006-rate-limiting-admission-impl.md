# ACT-006: Actor-Local Rate Limiting and Admission Policy — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-actor token-bucket rate limiting on the consumption (dequeue) side and a configurable admission policy chain on the enqueue side, giving operators fine-grained control over actor message processing without changing actor code.

**Architecture:** A new `ActorRateLimiter` (token bucket) is checked at the start of `MPSCActorMailbox::dequeue()`. A new `IAdmissionPolicy` chain is evaluated at the start of `MPSCActorMailbox::try_push()`. Both are optional per-actor with system-default TOML configuration, wired through the existing bootstrap engine. Metrics, CLI commands, and snapshot extensions expose runtime state.

**Tech Stack:** C++20, CMake/Ninja, GoogleTest, HPActor mailbox/actor/CLI/TOML runtime, no exceptions, no RTTI.

**Spec:** `docs/superpowers/specs/2026-06-05-act006-rate-limiting-admission-design.md`

---

## Source Design Inputs

- Spec: `docs/superpowers/specs/2026-06-05-act006-rate-limiting-admission-design.md`
- Architecture backlog: `docs/architecture/production/architecture-requirement-backlog.md#L30` (ACT-006)
- Production reliability plane: `docs/architecture/production/production-reliability-plane.md`
- Existing mailbox types: `include/hpactor/mailbox/mailbox_policy.hpp` (MailboxConfig, EnqueueResult, EnqueueResultCode)
- Existing mailbox orchestrator: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` (MPSCActorMailbox)
- Existing multi-lane queue: `include/hpactor/mailbox/multi_lane_queue.hpp` (MultiLaneQueue)
- Existing backpressure signal gate: `include/hpactor/mailbox/detail/backpressure_signal_gate.hpp` (interval + escalation CAS rate limiter pattern)
- Existing actor runner: `include/hpactor/sched/actor_execution_engine.hpp` (BehaviorActorRunner)
- Actor runner impl: `src/sched/actor_execution_engine.cpp`
- Metrics events: `include/hpactor/metrics/metrics_event.hpp`
- CLI commands: `src/cli/commands/actor_commands.cpp`
- CLI types / snapshot: `include/hpactor/cli/cli_types.hpp` (MboxSnapshot)
- TOML config (existing patterns): `include/hpactor/config/topology_model.hpp`, `src/config/bootstrap_engine.cpp`, `src/config/parsers/`
- Existing failure reasons: `include/hpactor/types/failure_reason.hpp` (FailureReason::RejectedByPolicy, value 40)

## File Structure

### Create

| File | Purpose |
|------|---------|
| `include/hpactor/mailbox/actor_rate_limiter.hpp` | Token-bucket rate limiter: `ActorRateLimiter` class |
| `include/hpactor/mailbox/admission_policy.hpp` | `IAdmissionPolicy` interface, `AdmissionDecision` enum, `AdmissionPolicyResult` struct |
| `include/hpactor/mailbox/detail/policies/type_filter_policy.hpp` | `TypeFilterPolicy` — block by TypeTag |
| `include/hpactor/mailbox/detail/policies/sender_filter_policy.hpp` | `SenderFilterPolicy` — block by sender ActorId |
| `include/hpactor/mailbox/detail/policies/priority_threshold_policy.hpp` | `PriorityThresholdPolicy` — minimum priority floor |
| `include/hpactor/mailbox/detail/policies/size_limit_policy.hpp` | `SizeLimitPolicy` — max payload bytes |
| `include/hpactor/mailbox/detail/policies/per_sender_rate_policy.hpp` | `PerSenderRatePolicy` — independent token bucket per sender |
| `include/hpactor/config/rate_limiting_config.hpp` | `RateLimitingConfig`, `PerActorRateLimitSpec`, `AdmissionRuleSpec` structs |
| `src/config/parsers/rate_limiting_config_parser.cpp` | Self-registering TOML subsystem parser for `[system.rate_limiting]` and `[system.admission]` |
| `tests/unit/mailbox/test_actor_rate_limiter.cpp` | Unit tests for `ActorRateLimiter` token-bucket math |
| `tests/unit/mailbox/test_admission_policies.cpp` | Unit tests for each built-in admission policy |
| `tests/unit/mailbox/test_rate_limiter_mailbox.cpp` | Unit tests for rate limiter integrated into MPSCActorMailbox |
| `tests/unit/mailbox/test_admission_mailbox.cpp` | Unit tests for admission policy chain in MPSCActorMailbox |
| `tests/unit/config/test_rate_limiting_config.cpp` | Unit tests for TOML parser |
| `tests/integration/actor/test_rate_limiting_integration.cpp` | Integration: fast producer + rate-limited actor |
| `tests/integration/actor/test_admission_policy_integration.cpp` | Integration: admission rejects by type/sender/size |
| `tests/system/test_rate_limiting_topology.cpp` | System: TOML-configured rate limiting end-to-end |

### Modify

| File | Purpose |
|------|---------|
| `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` | Add `ActorRateLimiter` member + `set_rate_limiter()`; rate-limit gate in `dequeue()`; admission policy vector + gate in `try_push()`; snapshot extensions |
| `include/hpactor/mailbox/mailbox_policy.hpp` | Add `EnqueueResultCode::RateLimited` (value 12); add `FailureReason` mapping for `RejectedByPolicy` |
| `include/hpactor/cli/cli_types.hpp` | Extend `MboxSnapshot` with rate limiter + admission policy fields |
| `include/hpactor/sched/actor_execution_engine.hpp` | Add rate-limiter-aware wakeup scheduling integration |
| `include/hpactor/metrics/metrics_event.hpp` | Add `kRateLimitBlocked`, `kAdmissionRejected`, `kAdmissionDLQRouted`, `kPerSenderBucketCount` |
| `include/hpactor/config/topology_model.hpp` | Extend `ActorDef` with rate limiting + admission spec fields |
| `src/config/bootstrap_engine.cpp` | Wire rate limiter + admission policy construction during actor spawn |
| `src/actor/actor_system.cpp` | Wire rate limiting + admission config propagation to mailbox |
| `src/sched/actor_execution_engine.cpp` | Rate-limiter-aware wakeup scheduling in `BehaviorActorRunner::run()` |
| `src/cli/commands/actor_commands.cpp` | Add `/actor <id> rate` and `/actor <id> admission` CLI commands |
| `src/metrics/metrics_aggregator.cpp` | Handle new metric event types |
| `tests/unit/mailbox/CMakeLists.txt` | Add new test source files |
| `tests/unit/config/CMakeLists.txt` | Add `test_rate_limiting_config.cpp` |
| `tests/unit/cli/CMakeLists.txt` | Add CLI integration for rate/admission commands |
| `tests/integration/actor/CMakeLists.txt` | Add integration test source files |
| `tests/system/CMakeLists.txt` | Add system test source files |

## Current State Audit

### 2.1 Implemented and Verified

| Component | File | Status |
|-----------|------|--------|
| `MailboxConfig` (capacity, priority_levels, overflow_policy, watermarks, signal interval) | `mailbox/mailbox_policy.hpp` | Done |
| `EnqueueResult` / `EnqueueResultCode` (11 values: Accepted, AcceptedWithSoftPressure, Rejected, DroppedNewest, DroppedExisting, ReroutedToDeadLetter, ReroutedToOverflow, MailboxClosed, ActorNotFound, EndpointBackpressure, EndpointCircuitOpen) | `mailbox/mailbox_policy.hpp` | Done |
| `MPSCActorMailbox::try_push()` — full admission path with overflow handler dispatch | `mailbox/mpsc_actor_mailbox.hpp` | Done |
| `MPSCActorMailbox::dequeue()` — consumer lock, lane dequeue, reservation release, overflow drain | `mailbox/mpsc_actor_mailbox.hpp` | Done |
| `MultiLaneQueue<T>` — system lane + N user priority lanes, `try_drop_from_lowest_user_lane()` | `mailbox/multi_lane_queue.hpp` | Done |
| `ReservationManager` — atomic slot + byte reservation | `mailbox/detail/reservation_manager.hpp` | Done |
| `PressureStateMachine` — Normal/Soft/Hard/Recovering transitions | `mailbox/detail/pressure_state_machine.hpp` | Done |
| `BackpressureSignalGate` — interval + escalation CAS-based signal rate limiter | `mailbox/detail/backpressure_signal_gate.hpp` | Done |
| Overflow handlers (Reject, DropNewest, DropOldest, DropLowestPriority, DeadLetter, Spill, SignalOnly) | `mailbox/detail/handlers/*.hpp` | Done |
| `BehaviorActorRunner::run()` — dequeue one message per activation, call `actor.receive()`, requeue if non-empty | `sched/actor_execution_engine.cpp` | Done |
| `ActorReadyGate` — CAS-based readiness gate | `sched/actor_ready_gate.hpp` | Done |
| `FailureReason::RejectedByPolicy` (value 40) | `types/failure_reason.hpp` | Done (unused) |
| `MboxSnapshot` — depth, capacity, pressure, lane depths, overflow stats | `cli/cli_types.hpp` | Done |
| Self-registering TOML subsystem parser pattern (`TomlSystemParserRegistration<T>`) | `config/toml_parser_registry.hpp` | Done |
| `BootstrapEngine::execute()` — spawn actors from TopologyModel | `config/bootstrap_engine.cpp` | Done |
| `MetricEventType` / `MpscRingBuffer` — lock-free metrics | `metrics/metrics_event.hpp`, `metrics_ring_buffer.hpp` | Done |

### 2.2 NOT Yet Implemented (Gaps)

| Gap | Description | Priority |
|-----|-------------|----------|
| **G1: ActorRateLimiter class** | No token-bucket rate limiter exists for actor consumption control | P0 |
| **G2: Rate limiter gate in dequeue()** | `MPSCActorMailbox::dequeue()` does not check rate limiter before consuming | P0 |
| **G3: Actor runner integration** | `BehaviorActorRunner::run()` needs rate-limiter-aware requeue logic | P0 |
| **G4: IAdmissionPolicy interface + built-in policies** | No admission policy interface exists. Need TypeFilter, SenderFilter, PriorityThreshold, SizeLimit, PerSenderRate policies | P0 |
| **G5: Admission policy gate in try_push()** | `MPSCActorMailbox::try_push()` has no admission policy evaluation before reservation | P0 |
| **G6: MboxSnapshot extensions** | `MboxSnapshot` has no rate limiter or admission policy fields | P0 |
| **G7: Metrics events** | No `kRateLimitBlocked`, `kAdmissionRejected`, `kAdmissionDLQRouted`, `kPerSenderBucketCount` metric events | P0 |
| **G8: CLI commands** | No `/actor <id> rate` or `/actor <id> admission` CLI commands | P0 |
| **G9: TOML config parser** | No parser for `[system.rate_limiting]` or `[system.admission]` sections | P0 |
| **G10: Per-actor config propagation** | `TopologyModel::ActorDef` and `BootstrapEngine` have no rate-limit / admission fields | P0 |
| **G11: Unit tests** | No unit tests for rate limiter, admission policies, or integrated mailbox behavior | P0 |
| **G12: Integration/system tests** | No tests exercising rate limiting or admission policy end-to-end with a live actor system | P0 |
| **G13: WakeupTag for precise scheduling** | No lightweight wakeup message type for rate-limiter-triggered timer-based requeue | P2 |

## 3. Gap Detail & Implementation Design

### 3.1 G1: ActorRateLimiter — Token-Bucket Implementation

**File:** `include/hpactor/mailbox/actor_rate_limiter.hpp` (NEW)

The class implements a token-bucket rate limiter. The key design decisions from the spec:

- **Tokens represent message-processing capacity.** Each `try_consume()` call debits one token.
- **Refill is time-driven.** On each `try_consume()` call, elapsed time since last refill is converted to earned tokens via `rate_per_ns_`.
- **Burst limit** is `max_tokens_` — tokens cannot accumulate beyond this.
- **Disabled state**: when `rate_per_sec <= 0`, enabled_ is false, and `try_consume()` unconditionally returns `true`.

```cpp
class ActorRateLimiter {
public:
    ActorRateLimiter() noexcept = default;

    void configure(double rate_per_sec, uint32_t burst) noexcept;
    bool try_consume(uint64_t now_ns) noexcept;
    uint64_t time_until_next_token_ns(uint64_t now_ns) const noexcept;
    double current_tokens() const noexcept;
    double configured_rate() const noexcept;
    uint32_t configured_burst() const noexcept;
    bool is_enabled() const noexcept;

private:
    std::atomic<double> tokens_{0.0};
    std::atomic<uint64_t> last_refill_ns_{0};
    double rate_per_ns_{0.0};
    uint32_t burst_{0};
    double max_tokens_{0.0};
    bool enabled_{false};
};
```

**Thread safety notes:**
- `try_consume()` is called under the mailbox's consumer lock (single-writer context). The non-atomic read/write of `tokens_` and `last_refill_ns_` is safe because only the consumer thread writes these.
- Fields are `std::atomic<>` for safe reads from CLI/metrics/admin threads.
- `configure()` uses atomic store — safe from any thread.

**Precision:** Use `std::chrono::steady_clock::time_point` internally for last refill tracking; convert to ns for computation. All arithmetic is double-precision.

### 3.2 G2 + G3: Rate Limiter Gate in dequeue() + Actor Runner

**Files:** `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` (modify), `src/sched/actor_execution_engine.cpp` (modify)

**dequeue() integration:**

```cpp
T* dequeue() noexcept {
    lock_consumer();

    // ── Rate limiter gate ──────────────────────────────────
    if (rate_limiter_ && rate_limiter_->is_enabled()) [[unlikely]] {
        uint64_t now_ns = steady_now_ns();
        if (!rate_limiter_->try_consume(now_ns)) {
            rate_limit_blocked_total_.fetch_add(1, std::memory_order_relaxed);
            if (metrics_ring_buffer_) [[unlikely]] {
                metrics::MetricEvent evt{};
                evt.actor_id = actor_id_;
                evt.event_type = metrics::MetricEventType::kRateLimitBlocked;
                evt.value_hi = 1;
                metrics_ring_buffer_->try_push(evt);
            }
            unlock_consumer();
            return nullptr;
        }
    }
    // ── End rate limiter gate ──────────────────────────────

    // ... existing dequeue logic ...
}
```

**BehaviorActorRunner integration:**

The runner already handles the case where `try_pop()` returns `false` while the mailbox is non-empty — it requeues the actor. For v1, this is sufficient. The rate limiter wakes naturally as follows:

1. Rate limiter blocks dequeue → dequeue returns nullptr → try_pop returns false
2. Runner checks `!mailbox->empty()` → true (message still in mailbox)
3. Runner calls `ready_gate_.mark_ready_already_admitted(actor)` → actor requeued
4. Scheduler re-activates the actor → run() → dequeue() → rate limiter may or may not admit
5. When rate limiter admits, message is dequeued and processed

This means the polling frequency is determined by the scheduler's work-stealing loop. For most cases this is adequate. The precise wakeup optimization (G13) is deferred to P2.

**ActorState interaction:** When rate-limited and mailbox is non-empty, the actor stays in Running state (requeued as ready). When rate-limited and mailbox is empty, the actor transitions to Idle.

### 3.3 G4: IAdmissionPolicy Interface + Built-in Policies

**File:** `include/hpactor/mailbox/admission_policy.hpp` (NEW)

```cpp
enum class AdmissionDecision : uint8_t {
    Accept,
    Reject,
    RerouteToDLQ,
};

struct AdmissionPolicyResult {
    AdmissionDecision decision{AdmissionDecision::Accept};
    FailureReason reason{FailureReason::RejectedByPolicy};
    const char* policy_name{nullptr};
    uint32_t match_code{0};
};

class IAdmissionPolicy {
public:
    virtual ~IAdmissionPolicy() = default;
    virtual AdmissionPolicyResult evaluate(
        const TypedMessage& msg,
        const MailboxEnvelopeMeta& meta,
        const MailboxConfig& config,
        uint64_t mailbox_depth) noexcept = 0;
    virtual const char* name() const noexcept = 0;
};
```

**Built-in policies** (files in `include/hpactor/mailbox/detail/policies/`):

| Policy | File | Match Logic | Decision |
|--------|------|-------------|----------|
| `TypeFilterPolicy` | `type_filter_policy.hpp` | Checks `meta.type_tag` against allowed/blocked sets | Reject if in blocked set or not in allowed set (when set non-empty) |
| `SenderFilterPolicy` | `sender_filter_policy.hpp` | Checks `meta.sender` against blocked set | Reject if sender in blocked set |
| `PriorityThresholdPolicy` | `priority_threshold_policy.hpp` | Checks `meta.priority < min_priority` | Reject (or RerouteToDLQ) if below threshold |
| `SizeLimitPolicy` | `size_limit_policy.hpp` | Checks `meta.estimated_bytes > max_bytes` | Reject or RerouteToDLQ if exceeds |
| `PerSenderRatePolicy` | `per_sender_rate_policy.hpp` | Per-sender token bucket, independently rate-limited | Reject when per-sender rate exceeded |

**PerSenderRatePolicy implementation details:**
- Uses a concurrent map (`ActorId → SenderBucket`), bounded by `max_senders_`
- Each `SenderBucket` is an independent token bucket with the same refill logic as `ActorRateLimiter`
- Lazy eviction: stale buckets (no activity for > 2x refill period) are pruned on the next `evaluate()` call that encounters them, or after the map exceeds `max_senders_ * 2` entries
- Thread safety: evaluate is called from the producer thread (any thread). The concurrent map uses per-bucket CAS for token updates.

### 3.4 G5: Admission Policy Gate in try_push()

**File:** `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` (modify)

```cpp
EnqueueResult try_push(T&& msg, const MailboxEnvelopeMeta& meta) noexcept {
    // ── Admission policy gate ──────────────────────────────
    if (admission_policies_ && !admission_policies_->empty()) [[unlikely]] {
        auto result = evaluate_policy_chain(msg, meta);
        if (result.decision != AdmissionDecision::Accept) {
            auto code = (result.decision == AdmissionDecision::RerouteToDLQ)
                            ? EnqueueResultCode::ReroutedToDeadLetter
                            : EnqueueResultCode::Rejected;
            total_admission_rejected_.fetch_add(1, std::memory_order_relaxed);
            if (metrics_ring_buffer_) [[unlikely]] {
                metrics::MetricEvent evt{};
                evt.actor_id = actor_id_;
                evt.event_type = (result.decision == AdmissionDecision::RerouteToDLQ)
                                     ? metrics::MetricEventType::kAdmissionDLQRouted
                                     : metrics::MetricEventType::kAdmissionRejected;
                evt.code = static_cast<uint8_t>(result.reason);
                evt.value_hi = 1;
                metrics_ring_buffer_->try_push(evt);
            }
            EnqueueResult r;
            r.code = code;
            r.target = actor_id_;
            r.failure_reason = result.reason;
            r.depth = static_cast<uint32_t>(lanes_.total_depth());
            r.capacity = config_.capacity.max_messages;
            r.pressure_state = pressure_state_.current_state();
            return r;
        }
    }
    // ── End admission policy gate ──────────────────────────

    // ... existing system lane check, reservation, overflow, enqueue ...
}
```

**Policy evaluation order**: linear scan of the policy vector, first rejection short-circuits. The `evaluate_policy_chain()` helper:

```cpp
AdmissionPolicyResult evaluate_policy_chain(const TypedMessage& msg,
                                            const MailboxEnvelopeMeta& meta) noexcept {
    for (const auto& policy : *admission_policies_) {
        auto result = policy->evaluate(msg, meta, config_,
                                       static_cast<uint64_t>(lanes_.total_depth()));
        if (result.decision != AdmissionDecision::Accept) {
            return result;
        }
    }
    return AdmissionPolicyResult{};
}
```

### 3.5 G6: MboxSnapshot Extensions

**File:** `include/hpactor/cli/cli_types.hpp` (modify)

```cpp
struct MboxSnapshot {
    // ... existing fields ...

    // Rate limiter fields (new)
    bool rate_limiter_enabled{false};
    double rate_limiter_rate{0.0};
    uint32_t rate_limiter_burst{0};
    double rate_limiter_current_tokens{0.0};
    uint64_t rate_limit_blocked_total{0};

    // Admission policy fields (new)
    uint8_t admission_policy_count{0};
    uint64_t admission_rejected_total{0};
    uint64_t admission_dlq_routed_total{0};
};
```

**Populated in `MPSCActorMailbox::snapshot()`:**

```cpp
cli::MboxSnapshot snapshot() const {
    auto s = /* existing snapshot construction */;

    // Rate limiter fields
    if (rate_limiter_) {
        s.rate_limiter_enabled = rate_limiter_->is_enabled();
        s.rate_limiter_rate = rate_limiter_->configured_rate();
        s.rate_limiter_burst = rate_limiter_->configured_burst();
        s.rate_limiter_current_tokens = rate_limiter_->current_tokens();
    }
    s.rate_limit_blocked_total = rate_limit_blocked_total_.load(std::memory_order_acquire);

    // Admission policy fields
    if (admission_policies_) {
        s.admission_policy_count = static_cast<uint8_t>(admission_policies_->size());
    }
    s.admission_rejected_total = total_admission_rejected_.load(std::memory_order_acquire);
    s.admission_dlq_routed_total = total_admission_dlq_routed_.load(std::memory_order_acquire);

    return s;
}
```

### 3.6 G7: Metrics Events

**File:** `include/hpactor/metrics/metrics_event.hpp` (modify)

Add to `MetricEventType` enum:

```cpp
kRateLimitBlocked   = 27,   // message deferred due to actor rate limit (counter)
kAdmissionRejected  = 28,   // message rejected by admission policy (counter)
kAdmissionDLQRouted = 29,   // message rerouted to DLQ by admission policy (counter)
kPerSenderBucketCount = 30, // number of active per-sender buckets (gauge)
```

**Metric name mapping** (in `metrics_formatter.cpp`, or wherever the formatter maps event types to OpenMetrics names):

```
kRateLimitBlocked   → hpactor_actor_rate_limit_blocked_total
kAdmissionRejected  → hpactor_actor_admission_rejected_total
kAdmissionDLQRouted → hpactor_actor_admission_dlq_routed_total
kPerSenderBucketCount → hpactor_actor_per_sender_buckets
```

### 3.7 G8: CLI Commands

**File:** `src/cli/commands/actor_commands.cpp` (modify)

New commands:

```
/actor <id> rate           — Show rate limiter state (enabled, rate, burst, tokens, blocked_total)
/actor <id> admission      — Show admission policy chain (policies, reject count per policy)
```

**Implementation pattern:**

```cpp
// In the command registration function for /actor subtree:
{
    auto rate_cmd = std::make_unique<CommandNode>("rate");
    rate_cmd->set_handler([](const CommandContext& ctx) {
        // Parse actor ID from parent context
        // Call InspectStateRequest with include_rate_limiter = true
        // Format output via tabular_formatter
    });
    actor_node->add_child(std::move(rate_cmd));
}

{
    auto admission_cmd = std::make_unique<CommandNode>("admission");
    admission_cmd->set_handler([](const CommandContext& ctx) {
        // Parse actor ID from parent context
        // Call InspectStateRequest with include_admission = true
        // Format output via tabular_formatter
    });
    actor_node->add_child(std::move(admission_cmd));
}
```

The `InspectStateRequest/Reply` protobuf protocol is extended with fields for rate limiter and admission policy state. This follows the same pattern as the circuit breaker CLI integration in ACT-005 (G7).

### 3.8 G9 + G10: TOML Config Parser + Per-Actor Propagation

**Files:**
- `include/hpactor/config/rate_limiting_config.hpp` (NEW)
- `src/config/parsers/rate_limiting_config_parser.cpp` (NEW)
- `include/hpactor/config/topology_model.hpp` (modify)
- `src/config/bootstrap_engine.cpp` (modify)
- `src/actor/actor_system.cpp` (modify)

**RateLimitingConfig structures:**

```cpp
// include/hpactor/config/rate_limiting_config.hpp

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
    double per_sender_rate{0.0};
    uint32_t per_sender_burst{0};
    uint32_t max_senders{256};
};

struct RateLimitingConfig {
    PerActorRateLimitSpec system_default;
    AdmissionRuleSpec system_admission;
};
```

**TOML format** (from spec):

```toml
[system.rate_limiting]
enabled = false
default_rate = 100.0
default_burst = 10

[system.admission]
enabled = false

[system.admission.rules]
type_blocked = ["DebugMessageTag", "HealthCheckTag"]

[[system.admission.per_sender]]
rate = 50.0
burst = 5
max_senders = 256

[[actor]]
name = "my_actor"
behavior = "order_handler"

[actor.rate_limiting]
enabled = true
rate = 500.0
burst = 50

[actor.admission]
enabled = true

[actor.admission.rules]
type_allowed = ["OrderRequestTag", "PaymentResultTag"]
priority_min = 2
size_max_bytes = 65536
```

**Config propagation flow:**

1. `RateLimitingConfigParser` (self-registering subsystem parser) parses `[system.rate_limiting]` and `[system.admission]` into `RateLimitingConfig`
2. `TopologyModel` stores per-actor overrides in `ActorDef::rate_limit` and `ActorDef::admission` fields
3. `BootstrapEngine::execute()` passes `RateLimitingConfig` to `ActorSystem::spawn_actor_internal()` 
4. `ActorSystem` creates `ActorRateLimiter` and policy chain from merged config (per-actor overrides win over system defaults)
5. The created objects are wired into the mailbox via `set_rate_limiter()` / `set_admission_policies()`

### 3.9 G11 + G12: Test Plan

See Section 6 for the full test plan organized by RED→GREEN phase.

### 3.10 G13: Precise Wakeup Scheduling (P2, deferred)

The design spec describes a `WakeupTag` (TypeTag 34) and `TypedMessage::create_wakeup()` for precise timer-based requeue when rate-limited. This optimization prevents busy polling in the scheduler. Deferred to P2 because:

- The scheduler's existing work-stealing loop provides adequate interleaving for most cases
- The optimization adds complexity (new system TypeTag, timer management)
- Measurement should guide whether it's needed

## 4. Data Flow

### 4.1 Normal Flow (no rate limit, no admission policy)

```
Producer: try_send(target, msg)
  → ActorSystem::try_deliver_local(target, msg, meta)
    → MPSCActorMailbox::try_push(msg, meta)
      → [no admission policies → skip]
      → reservation_.try_reserve(...) → Reserved
      → lanes_.enqueue(node, lane)
      → update_pressure_state()
      → scheduler_->notify_ready()
      → EnqueueResult{Accepted}

Scheduler: BehaviorActorRunner::run()
  → actor_state: Ready → Running
  → mailbox->try_pop(msg)
    → MPSCActorMailbox::dequeue()
      → [no rate limiter → skip]
      → lanes_.dequeue() → node
      → reservation_.release(bytes)
      → return node
  → actor.receive(msg)
  → [mailbox non-empty?] → RequeueReady
```

### 4.2 Rate-Limited Flow

```
Producer: try_send(target, msg)
  → [normal admission, enqueued]

Scheduler: BehaviorActorRunner::run()
  → mailbox->try_pop(msg)
    → MPSCActorMailbox::dequeue()
      → rate_limiter_->try_consume(now_ns) → false
      → emit kRateLimitBlocked metric
      → unlock_consumer()
      → return nullptr
  → try_pop returned false
  → [mailbox non-empty?] → yes (message still there)
  → ready_gate_.mark_ready_already_admitted() → AlreadyReady
  → RequeueReady
  → [scheduler re-activates, loop continues until rate limiter admits]
```

### 4.3 Admission-Policy-Rejected Flow

```
Producer: try_send(target, msg)
  → ActorSystem::try_deliver_local(target, msg, meta)
    → MPSCActorMailbox::try_push(msg, meta)
      → evaluate_policy_chain(msg, meta)
        → TypeFilterPolicy::evaluate() → Reject
      → emit kAdmissionRejected metric
      → total_admission_rejected_++
      → EnqueueResult{Rejected, failure_reason=RejectedByPolicy}
  → caller sees RejectedByPolicy
```

### 4.4 Per-Sender Rate-Limited Flow

```
Sender A: try_send(target, msg)
  → MPSCActorMailbox::try_push(msg, meta)
    → PerSenderRatePolicy::evaluate(msg, meta)
      → lookup bucket for meta.sender (ActorId of Sender A)
      → try_consume() → false (sender A exceeded rate)
      → Reject
  → EnqueueResult{Rejected, failure_reason=RejectedByPolicy}

Sender B: try_send(target, msg) (simultaneously)
  → PerSenderRatePolicy::evaluate(msg, meta)
    → lookup bucket for meta.sender (ActorId of Sender B)
    → try_consume() → true (sender B within rate)
    → Accept
  → message admitted normally
```

### 4.5 Rate Limiting + Backpressure + Overflow Interaction

```
1. Actor configured with rate=10/s, burst=5, mailbox capacity=1024
2. Producer sends 1000 messages rapidly
3. Mailbox fills to capacity (reservation exhausted)
4. Overflow handler activates: RejectNewest or DropOldest
5. Backpressure signal gate emits pressure signals to producers
6. Rate limiter slowly drains mailbox at 10 msg/s
7. Producers see EnqueueResult::Rejected or EnqueueResult::AcceptedWithSoftPressure
```

This is correct behavior — the rate limiter prevents fast consumption, and the existing bounded mailbox prevents unbounded growth. Operators should tune capacity and rate together.

## 5. New and Modified Files

### New Files

| File | Purpose |
|------|---------|
| `include/hpactor/mailbox/actor_rate_limiter.hpp` | Token-bucket `ActorRateLimiter` class |
| `include/hpactor/mailbox/admission_policy.hpp` | `IAdmissionPolicy` interface + `AdmissionDecision` + `AdmissionPolicyResult` |
| `include/hpactor/mailbox/detail/policies/type_filter_policy.hpp` | TypeTag-based admission filter |
| `include/hpactor/mailbox/detail/policies/sender_filter_policy.hpp` | Sender ActorId-based admission filter |
| `include/hpactor/mailbox/detail/policies/priority_threshold_policy.hpp` | Priority-based admission filter |
| `include/hpactor/mailbox/detail/policies/size_limit_policy.hpp` | Size-based admission filter |
| `include/hpactor/mailbox/detail/policies/per_sender_rate_policy.hpp` | Per-sender token-bucket admission policy |
| `include/hpactor/config/rate_limiting_config.hpp` | `RateLimitingConfig`, `PerActorRateLimitSpec`, `AdmissionRuleSpec` structs |
| `src/config/parsers/rate_limiting_config_parser.cpp` | Self-registering TOML subsystem parser |
| `tests/unit/mailbox/test_actor_rate_limiter.cpp` | Unit tests for ActorRateLimiter |
| `tests/unit/mailbox/test_admission_policies.cpp` | Unit tests for built-in admission policies |
| `tests/unit/mailbox/test_rate_limiter_mailbox.cpp` | Unit tests for rate limiter in MPSCActorMailbox |
| `tests/unit/mailbox/test_admission_mailbox.cpp` | Unit tests for admission policies in MPSCActorMailbox |
| `tests/unit/config/test_rate_limiting_config.cpp` | Unit tests for TOML parser |
| `tests/integration/actor/test_rate_limiting_integration.cpp` | Integration: fast producer + rate-limited actor |
| `tests/integration/actor/test_admission_policy_integration.cpp` | Integration: policy rejection by type/sender/size |
| `tests/system/test_rate_limiting_topology.cpp` | System: TOML-configured end-to-end |

### Modified Files

| File | Change |
|------|--------|
| `include/hpactor/mailbox/mailbox_policy.hpp` | Add `EnqueueResultCode::RateLimited` (12); `EnqueueResult::failure_reason()` mapping for `RejectedByPolicy` |
| `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` | Add rate limiter members + gate in `dequeue()`; admission policy members + gate in `try_push()`; snapshot extensions; atomic counters for blocked/rejected |
| `include/hpactor/cli/cli_types.hpp` | Extend `MboxSnapshot` with rate + admission fields |
| `include/hpactor/sched/actor_execution_engine.hpp` | Add `steady_now_ns()` helper; rate-limiter-aware wakeup in `BehaviorActorRunner` |
| `include/hpactor/metrics/metrics_event.hpp` | Add 4 new metric event type values (27-30) |
| `include/hpactor/config/topology_model.hpp` | Add `rate_limit` and `admission` fields to `ActorDef` |
| `src/config/bootstrap_engine.cpp` | Wire rate limiter + policy construction during spawn |
| `src/actor/actor_system.cpp` | Propagate rate limit and admission config to mailbox on spawn |
| `src/sched/actor_execution_engine.cpp` | Rate-limiter-aware requeue logic in `BehaviorActorRunner::run()` |
| `src/cli/commands/actor_commands.cpp` | Add `/actor <id> rate` and `/actor <id> admission` handlers |
| `src/metrics/metrics_aggregator.cpp` | Handle `kRateLimitBlocked`, `kAdmissionRejected`, `kAdmissionDLQRouted`, `kPerSenderBucketCount` |
| `tests/unit/mailbox/CMakeLists.txt` | Add 4 new test source files |
| `tests/unit/config/CMakeLists.txt` | Add `test_rate_limiting_config.cpp` |
| `tests/unit/cli/CMakeLists.txt` | Add CLI rate/admission command test(s) |
| `tests/integration/actor/CMakeLists.txt` | Add 2 new test source files |
| `tests/system/CMakeLists.txt` | Add `test_rate_limiting_topology.cpp` |

## 6. Test Plan

### 6.1 Phase 1: Core Token Bucket (RED→GREEN)

**Test file:** `tests/unit/mailbox/test_actor_rate_limiter.cpp`

**Test 1.1: `RateLimiterTest.SteadyRateLimitsConsumption`**
- Setup: `ActorRateLimiter` with `rate=100.0, burst=10`
- Action: Call `try_consume()` 200 times, spaced 5ms apart (simulating the elapsed time between calls)
- Assert: At most 100 messages are admitted per second (with ±5% tolerance for timing drift). Over 2 seconds at 5ms intervals (400 calls), at most ~200 messages are admitted.

**Test 1.2: `RateLimiterTest.BurstAllowsSpike`**
- Setup: `rate=10.0, burst=20`
- Action: Call `try_consume()` 20 times in rapid succession (no elapsed time between calls), then call more slowly
- Assert: First 20 calls succeed (burst consumed), subsequent calls fail until enough time elapses for refill

**Test 1.3: `RateLimiterTest.IdleRefill`**
- Setup: `rate=100.0, burst=10`
- Action: Consume 10 tokens (exhaust burst), wait 100ms, call `try_consume()`
- Assert: After 100ms idle at 100 msg/s rate, ~10 tokens have accumulated. `try_consume()` succeeds.

**Test 1.4: `RateLimiterTest.DisabledWhenRateZero`**
- Setup: `configure(0.0, 0)`
- Action: Call `try_consume()` 10000 times in a tight loop
- Assert: All calls return true (no rate limiting)

**Test 1.5: `RateLimiterTest.CurrentTokensReflectsState`**
- Setup: `rate=10.0, burst=5`, consume 3 tokens
- Assert: `current_tokens()` returns ~2.0 (within floating-point tolerance)

**Test 1.6: `RateLimiterTest.TimeUntilNextToken`**
- Setup: `rate=10.0, burst=5`, consume all 5 tokens, record `now_ns`
- Assert: `time_until_next_token_ns(now_ns)` returns ~100ms (1/10 second)

**Test 1.7: `RateLimiterTest.TimeUntilNextTokenReturnsMaxWhenDisabled`**
- Setup: `rate=0.0, burst=0`
- Assert: `time_until_next_token_ns(now)` returns `UINT64_MAX`

**RED:** Run `./build/tests/unit/mailbox/test_actor_rate_limiter` — compilation fails (no file)
**GREEN:** Implement `ActorRateLimiter` — all 7 tests pass
**REFACTOR:** Verify thread-safety documentation matches implementation

### 6.2 Phase 2: Rate Limiter in Mailbox (RED→GREEN)

**Test file:** `tests/unit/mailbox/test_rate_limiter_mailbox.cpp`

**Test 2.1: `RateLimiterMailboxTest.RateLimitedDequeueReturnsNullptr`**
- Setup: `MPSCActorMailbox` with `rate_limiter_` set to `rate=0.1, burst=0` (1 msg every 10s)
- Action: Inject one message via `inject_for_test()`. Call `try_pop()`.
- Assert: Returns `false` (rate limited). Mailbox is non-empty.

**Test 2.2: `RateLimiterMailboxTest.RateAdmittedDequeueReturnsMessage`**
- Setup: `MPSCActorMailbox` with `rate_limiter_` set to `rate=1000000, burst=1000000` (effectively unlimited)
- Action: Inject one message via `inject_for_test()`. Call `try_pop()`.
- Assert: Returns `true`, message matches what was injected.

**Test 2.3: `RateLimiterMailboxTest.SnapshotReflectsRateLimiterState`**
- Setup: MPSCActorMailbox with rate limiter configured to `rate=50.0, burst=10`
- Action: `snapshot()`
- Assert: `rate_limiter_enabled == true`, `rate_limiter_rate == 50.0`, `rate_limiter_burst == 10`, `rate_limiter_current_tokens == 10.0`

**Test 2.4: `RateLimiterMailboxTest.NoRateLimiterSnapshotShowsDisabled`**
- Setup: MPSCActorMailbox without rate limiter (default)
- Assert: `snapshot().rate_limiter_enabled == false`

**Test 2.5: `RateLimiterMailboxTest.BlockedCounterIncrements`**
- Setup: MPSCActorMailbox with rate limiter set to `rate=0.1, burst=0`
- Action: Call `try_pop()` 5 times
- Assert: `snapshot().rate_limit_blocked_total == 5`

**RED:** `test_rate_limiter_mailbox.cpp` — 5 tests fail (no implementation)
**GREEN:** Implement rate limiter gate in `dequeue()` — all 5 tests pass
**REFACTOR:** Check snapshot and counter consistency

### 6.3 Phase 3: Admission Policy Interface + Built-in Policies (RED→GREEN)

**Test file:** `tests/unit/mailbox/test_admission_policies.cpp`

**Test 3.1: `AdmissionPolicyTest.TypeFilterRejectsBlockedTag`**
- Setup: `TypeFilterPolicy` with blocked set containing `DebugMessageTag` (value 99)
- Action: `evaluate()` with `meta.type_tag = DebugMessageTag`
- Assert: `decision == AdmissionDecision::Reject`, `reason == RejectedByPolicy`

**Test 3.2: `AdmissionPolicyTest.TypeFilterAcceptsAllowedTag`**
- Setup: `TypeFilterPolicy` with allowed set {`OrderRequestTag`}
- Action: `evaluate()` with `meta.type_tag = OrderRequestTag`
- Assert: `decision == AdmissionDecision::Accept`

**Test 3.3: `AdmissionPolicyTest.TypeFilterRejectsNotInAllowedSet`**
- Setup: `TypeFilterPolicy` with allowed set {`OrderRequestTag`}
- Action: `evaluate()` with `meta.type_tag = SomeOtherTag`
- Assert: `decision == AdmissionDecision::Reject`

**Test 3.4: `AdmissionPolicyTest.TypeFilterEmptyAllowedBlockedAcceptsAll`**
- Setup: `TypeFilterPolicy` with no allowed or blocked tags
- Action: `evaluate()` with any tag
- Assert: `decision == AdmissionDecision::Accept`

**Test 3.5: `AdmissionPolicyTest.SenderFilterRejectsBlockedSender`**
- Setup: `SenderFilterPolicy` with blocked set containing `ActorId(42)`
- Action: `evaluate()` with `meta.sender = ActorId(42)`
- Assert: `decision == AdmissionDecision::Reject`

**Test 3.6: `AdmissionPolicyTest.SenderFilterAcceptsNonBlockedSender`**
- Setup: `SenderFilterPolicy` with blocked set containing `ActorId(42)`
- Action: `evaluate()` with `meta.sender = ActorId(99)`
- Assert: `decision == AdmissionDecision::Accept`

**Test 3.7: `AdmissionPolicyTest.PriorityThresholdRejectsBelow`**
- Setup: `PriorityThresholdPolicy(min_priority = 3)`
- Action: `evaluate()` with `meta.priority = 1`
- Assert: `decision == AdmissionDecision::Reject`

**Test 3.8: `AdmissionPolicyTest.PriorityThresholdAcceptsAtOrAbove`**
- Setup: `PriorityThresholdPolicy(min_priority = 3)`
- Action: `evaluate()` with `meta.priority = 3` and `meta.priority = 5`
- Assert: `decision == AdmissionDecision::Accept` for both

**Test 3.9: `AdmissionPolicyTest.SizeLimitRejectsOversized`**
- Setup: `SizeLimitPolicy(max_bytes = 1024)`
- Action: `evaluate()` with `meta.estimated_bytes = 2048`
- Assert: `decision == AdmissionDecision::Reject`

**Test 3.10: `AdmissionPolicyTest.SizeLimitAcceptsWithinLimit`**
- Setup: `SizeLimitPolicy(max_bytes = 1024)`
- Action: `evaluate()` with `meta.estimated_bytes = 512`
- Assert: `decision == AdmissionDecision::Accept`

**Test 3.11: `AdmissionPolicyTest.PerSenderRateLimitsIndependently`**
- Setup: `PerSenderRatePolicy(rate=10.0, burst=5, max_senders=10)`
- Action: Send 10 messages from Sender A (ActorId 1) and 10 messages from Sender B (ActorId 2) in rapid succession
- Assert: Each sender is independently limited to 5 messages (burst) before rate limiting kicks in. After the burst, further messages from each sender are independently rejected.

**Test 3.12: `AdmissionPolicyTest.PerSenderMaxSendersEvictsStale`**
- Setup: `PerSenderRatePolicy(rate=10.0, burst=5, max_senders=2)`
- Action: Send from Sender A, B, C (3 unique senders)
- Assert: Only 2 buckets can coexist. The third sender may be rejected or evict an older bucket.

**Test 3.13: `AdmissionPolicyTest.ChainShortCircuitsOnFirstRejection`**
- Setup: Vector of policies: [SenderFilter(block A), TypeFilter(block DebugTag)]
- Action: `evaluate()` with sender=A, type=DebugTag
- Assert: First policy rejects. Second policy is never consulted.

**Test 3.14: `AdmissionPolicyTest.ChainAllAccept`**
- Setup: Vector of policies: [SenderFilter(block set=A), TypeFilter(block set=DebugTag)]
- Action: `evaluate()` with sender=B, type=OrderTag
- Assert: All policies accept. Result is Accept.

**RED:** `test_admission_policies.cpp` — 14 tests fail (no implementations)
**GREEN:** Implement `IAdmissionPolicy` interface + 5 built-in policies + `evaluate_policy_chain()` — all 14 tests pass
**REFACTOR:** Verify policy name strings match CLI expectations

### 6.4 Phase 4: Admission Policy Gate in Mailbox (RED→GREEN)

**Test file:** `tests/unit/mailbox/test_admission_mailbox.cpp`

**Test 4.1: `AdmissionMailboxTest.PolicyRejectsMessage`**
- Setup: `MPSCActorMailbox` with a `SenderFilterPolicy` that blocks sender `ActorId(42)`
- Action: `try_push(msg, meta{ sender=ActorId(42) })`
- Assert: Returns `EnqueueResult{Rejected}`, `failure_reason == RejectedByPolicy`

**Test 4.2: `AdmissionMailboxTest.PolicyReroutesToDLQ`**
- Setup: `MPSCActorMailbox` with a policy that returns `RerouteToDLQ`
- Action: `try_push(msg, meta)`
- Assert: Returns `EnqueueResult{ReroutedToDeadLetter}`

**Test 4.3: `AdmissionMailboxTest.NoPoliciesAcceptsNormally`**
- Setup: `MPSCActorMailbox` without admission policies
- Action: `try_push(msg, meta)`
- Assert: Returns `EnqueueResult{Accepted}` (normal admission path)

**Test 4.4: `AdmissionMailboxTest.ChainShortCircuitsInMailbox`**
- Setup: `MPSCActorMailbox` with two policies, first rejects
- Action: `try_push(msg, meta)`
- Assert: Returns rejection. Mailbox depth is 0 (no message leaked through).

**Test 4.5: `AdmissionMailboxTest.SnapshotShowsPolicyCount`**
- Setup: `MPSCActorMailbox` with 3 admission policies
- Assert: `snapshot().admission_policy_count == 3`

**RED:** `test_admission_mailbox.cpp` — 5 tests fail (no gate in try_push)
**GREEN:** Implement admission policy gate in `try_push()` — all 5 tests pass
**REFACTOR:** Verify counters match expected values

### 6.5 Phase 5: Metrics (RED→GREEN)

**Test file:** Existing metrics test infrastructure

**Test 5.1: Rate limiter emits kRateLimitBlocked on blocked dequeue**
- Setup: Rate-limited mailbox, inject message
- Action: `try_pop()`
- Assert: `kRateLimitBlocked` event appears in metrics ring buffer

**Test 5.2: Admission policy emits kAdmissionRejected on reject**
- Setup: `MPSCActorMailbox` with `SenderFilterPolicy` blocking sender ActorId(42)
- Action: `try_push(msg, meta{ sender=ActorId(42) })`
- Assert: `kAdmissionRejected` event appears in metrics ring buffer

**Test 5.3: Admission policy emits kAdmissionDLQRouted on DLQ reroute**
- Setup: `MPSCActorMailbox` with policy that returns RerouteToDLQ
- Action: `try_push(msg, meta)`
- Assert: `kAdmissionDLQRouted` event appears in metrics ring buffer

**RED:** Run metrics tests — fail (no metric event types)
**GREEN:** Add `kRateLimitBlocked`, `kAdmissionRejected`, `kAdmissionDLQRouted`, `kPerSenderBucketCount` to `MetricEventType` — all pass
**REFACTOR:** Verify metric names in OpenMetrics formatter

### 6.6 Phase 6: TOML Configuration (RED→GREEN)

**Test file:** `tests/unit/config/test_rate_limiting_config.cpp`

**Test 6.1: `RateLimitingConfigTest.ParsesSystemDefaults`**
- Setup: Minimal TOML with `[system.rate_limiting]` and `[system.admission]`
- Action: Parse via `RateLimitingConfigParser`
- Assert: `config.system_default.enabled == true`, `config.system_default.rate_per_sec == 100.0`, etc.

**Test 6.2: `RateLimitingConfigTest.SystemDefaultsOverride`**
- Setup: `[system.rate_limiting]\nenabled = false`
- Assert: `config.system_default.enabled == false`

**Test 6.3: `RateLimitingConfigTest.ParsesAdmissionRules`**
- Setup: TOML with `[system.admission.rules] type_blocked = ["DebugMessageTag"]`
- Action: Parse
- Assert: `config.system_admission.type_blocked_tags` contains the numeric TypeTag value for `DebugMessageTag`

**Test 6.4: `RateLimitingConfigTest.PerActorOverrides`**
- Setup: TOML with per-actor rate limiting section
- Action: Parse topology model
- Assert: `actor_def.rate_limit.enabled == true`, `actor_def.rate_limit.rate_per_sec == 500.0`

**Test 6.5: `RateLimitingConfigTest.MissingSectionDefaults`**
- Setup: TOML with no `[system.rate_limiting]` section
- Assert: All fields are their default values (disabled, zero rate)

**RED:** `test_rate_limiting_config.cpp` — compilation fails (no parser)
**GREEN:** Implement `RateLimitingConfigParser` — all 5 tests pass
**REFACTOR:** Verify parser follows existing self-registration pattern

### 6.7 Phase 7: Integration Tests (RED→GREEN)

**Test file:** `tests/integration/actor/test_rate_limiting_integration.cpp`

**Test 7.1: `RateLimitingIntegration.FastProducerRateLimitedConsumer`**
- Setup: Actor system with 1 scheduler thread. Actor with `rate_limiting{enabled=true, rate=50.0, burst=10}`. Producer sends 200 messages as fast as possible.
- Action: Wait 3 seconds. Measure mailbox depth and consumption rate.
- Assert: Consumption rate is approximately 50 msg/s (±20% tolerance for timing). Mailbox depth does not exceed capacity + burst.

**Test 7.2: `RateLimitingIntegration.NoRateLimitConsumesAll`**
- Setup: Same as 7.1 but rate limiting disabled
- Action: Send 200 messages, wait 1 second
- Assert: All 200 messages consumed

**Test file:** `tests/integration/actor/test_admission_policy_integration.cpp`

**Test 7.3: `AdmissionPolicyIntegration.TypeFilterBlocksDebugMessages`**
- Setup: Actor with `TypeFilterPolicy` blocking `DebugMessageTag`. Producer sends 10 normal messages + 10 debug messages.
- Action: Send all 20 messages
- Assert: Only 10 normal messages are admitted. 10 debug messages return `RejectedByPolicy`.

**Test 7.4: `AdmissionPolicyIntegration.SenderFilterIsolatesSenders`**
- Setup: Actor with `SenderFilterPolicy` blocking sender ActorId(42)
- Action: Send 5 messages from ActorId(42), 5 from ActorId(99)
- Assert: Messages from ActorId(42) are rejected. Messages from ActorId(99) are accepted.

**Test 7.5: `AdmissionPolicyIntegration.SizeLimitRejectsOversized`**
- Setup: Actor with `SizeLimitPolicy(max_bytes=1024)`
- Action: Send 5 messages with `estimated_bytes=512`, 5 with `estimated_bytes=2048`
- Assert: Small messages accepted. Large messages rejected.

**RED:** Integration tests — compilation fails (no implementation)
**GREEN:** Wire everything together — all pass
**REFACTOR:** Check no regressions in existing integration tests

### 6.8 Phase 8: System Tests (RED→GREEN)

**Test file:** `tests/system/test_rate_limiting_topology.cpp`

**Test 8.1: `RateLimitingTopology.TomlConfiguredRateLimiting`**
- Setup: Load TOML topology with `[actor.rate_limiting]` configuration
- Action: Actor spawns, producer sends messages
- Assert: Consumption rate matches configured rate

**Test 8.2: `RateLimitingTopology.TomlConfiguredAdmission`**
- Setup: Load TOML topology with `[actor.admission]` configuration
- Action: Send blocked messages and allowed messages
- Assert: Only allowed messages pass through

**RED:** System test — compilation fails
**GREEN:** Wire TOML → BootstrapEngine → ActorSystem → Mailbox — all pass
**REFACTOR:** Verify that TOML without rate_limiting section produces zero overhead (test via disabled-equivalent behavior)

## 7. Implementation Phases

### Phase 1 (Pre-work): Verify Existing Tests Pass

```bash
# Configure once (if needed)
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Verify all existing mailbox and actor tests pass before starting
ctest -R "Mailbox|Actor|Rate|Admission" --output-on-failure
```

### Phase 2: Core Token Bucket (G1)

1. Create `include/hpactor/mailbox/actor_rate_limiter.hpp` with `ActorRateLimiter` class
2. Create `tests/unit/mailbox/test_actor_rate_limiter.cpp` with Tests 1.1–1.7 (RED)
3. Implement `ActorRateLimiter` (GREEN)
4. Verify all 7 token-bucket unit tests pass

**Verification:**
```bash
ninja -C build test_actor_rate_limiter 2>/dev/null || ninja -C build tests/unit/mailbox/test_actor_rate_limiter
./build/tests/unit/mailbox/test_actor_rate_limiter
```

### Phase 3: Rate Limiter Gate in Mailbox (G2–G3)

1. Modify `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`:
   - Add `rate_limiter_` member (unique_ptr)
   - Add `set_rate_limiter()` method
   - Add rate-limit gate in `dequeue()`
   - Add `rate_limit_blocked_total_` atomic counter
   - Extend `snapshot()` with rate limiter fields
2. Create `tests/unit/mailbox/test_rate_limiter_mailbox.cpp` with Tests 2.1–2.5 (RED)
3. Implement integration (GREEN)
4. Update `include/hpactor/cli/cli_types.hpp` (MboxSnapshot extensions)
5. Update `include/hpactor/sched/actor_execution_engine.hpp` with `steady_now_ns()`
6. Modify `src/sched/actor_execution_engine.cpp` — rate-limiter-aware requeue logic
7. Verify all 5 mailbox rate limiter tests pass

**Verification:**
```bash
ninja -C build test_rate_limiter_mailbox
./build/tests/unit/mailbox/test_rate_limiter_mailbox
ctest -R "RateLimiter|ActorRateLimiter" --output-on-failure
```

### Phase 4: Admission Policy Interface + Built-in Policies (G4)

1. Create `include/hpactor/mailbox/admission_policy.hpp`:
   - `AdmissionDecision` enum
   - `AdmissionPolicyResult` struct
   - `IAdmissionPolicy` interface
2. Create built-in policy headers in `include/hpactor/mailbox/detail/policies/`:
   - `type_filter_policy.hpp`
   - `sender_filter_policy.hpp`
   - `priority_threshold_policy.hpp`
   - `size_limit_policy.hpp`
   - `per_sender_rate_policy.hpp`
3. Create `tests/unit/mailbox/test_admission_policies.cpp` with Tests 3.1–3.14 (RED)
4. Implement all 5 policies + chain evaluation (GREEN)
5. Verify all 14 admission policy tests pass

**Verification:**
```bash
ninja -C build test_admission_policies
./build/tests/unit/mailbox/test_admission_policies
```

### Phase 5: Admission Policy Gate in Mailbox (G5)

1. Modify `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`:
   - Add `admission_policies_` member (shared_ptr<vector>)
   - Add `set_admission_policies()` method
   - Add `evaluate_policy_chain()` helper
   - Add admission policy gate at top of `try_push()`
   - Add `total_admission_rejected_` and `total_admission_dlq_routed_` counters
   - Extend `snapshot()` with admission policy fields
2. Create `tests/unit/mailbox/test_admission_mailbox.cpp` with Tests 4.1–4.5 (RED)
3. Implement integration (GREEN)
4. Verify all 5 admission mailbox tests pass

**Verification:**
```bash
ninja -C build test_admission_mailbox
./build/tests/unit/mailbox/test_admission_mailbox
```

### Phase 6: Metrics (G7)

1. Modify `include/hpactor/metrics/metrics_event.hpp` — add 4 new `MetricEventType` values
2. Modify `src/metrics/metrics_aggregator.cpp` — handle new event types
3. Verify metric emission points in:
   - `MPSCActorMailbox::dequeue()` (rate limited) → `kRateLimitBlocked`
   - `MPSCActorMailbox::try_push()` (policy reject) → `kAdmissionRejected`
   - `MPSCActorMailbox::try_push()` (policy DLQ) → `kAdmissionDLQRouted`

**Verification:**
```bash
# Run existing metrics tests to confirm no regression
ctest -R "Metric" --output-on-failure
# Run rate + admission tests that verify metric events
./build/tests/unit/mailbox/test_rate_limiter_mailbox
./build/tests/unit/mailbox/test_admission_mailbox
```

### Phase 7: CLI Commands (G8)

1. Modify `src/cli/commands/actor_commands.cpp`:
   - Register `/actor <id> rate` command
   - Register `/actor <id> admission` command
   - Add protobuf fields to `InspectStateRequest` for rate/admission data
   - Add rate/admission data population in `InspectStateReply`
   - Wire into existing `EventBasedActor::receive()` InspectState handler
2. Add CLI tests

**Verification:**
```bash
ninja -C build test_actor_commands
./build/tests/unit/cli/test_actor_commands
```

### Phase 8: TOML Configuration (G9–G10)

1. Create `include/hpactor/config/rate_limiting_config.hpp` — spec structs
2. Create `src/config/parsers/rate_limiting_config_parser.cpp` — self-registering parser
3. Modify `include/hpactor/config/topology_model.hpp` — extend `ActorDef`
4. Modify `src/config/bootstrap_engine.cpp` — wire rate limiter + policy construction
5. Modify `src/actor/actor_system.cpp` — propagate config to mailbox
6. Create `tests/unit/config/test_rate_limiting_config.cpp` with Tests 6.1–6.5 (RED→GREEN)

**Verification:**
```bash
ninja -C build test_rate_limiting_config
./build/tests/unit/config/test_rate_limiting_config
ctest -R "RateLimiting|Admission" --output-on-failure
```

### Phase 9: Integration + System Tests (G11–G12)

1. Create `tests/integration/actor/test_rate_limiting_integration.cpp` — Tests 7.1–7.2
2. Create `tests/integration/actor/test_admission_policy_integration.cpp` — Tests 7.3–7.5
3. Create `tests/system/test_rate_limiting_topology.cpp` — Tests 8.1–8.2
4. Wire into respective `CMakeLists.txt` files

**Verification:**
```bash
ninja -C build test_rate_limiting_integration test_admission_policy_integration test_rate_limiting_topology 2>/dev/null || ninja -C build
ctest -R "RateLimiting|AdmissionPolicy|RateLimitingTopology" --output-on-failure -j4 --timeout 60
```

### Phase 10 (P2, deferred): Precise Wakeup Optimization (G13)

1. Add `TypeTag::WakeupTag` (value 34) to `include/hpactor/types/types.hpp`
2. Add `TypedMessage::create_wakeup()` factory
3. Handle `WakeupTag` as no-op in default behavior
4. Schedule wakeup timer in `BehaviorActorRunner::run()` when rate limited and `time_until_next_token_ns` returns a finite value

## 8. Targeted Verification Commands

If `build/` does not exist:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

### Phase-by-phase verification:

| Phase | Command |
|-------|---------|
| Pre-work | `ctest -R "Mailbox|Actor|Rate|Admission" --output-on-failure` |
| Phase 2 | `./build/tests/unit/mailbox/test_actor_rate_limiter` |
| Phase 3 | `./build/tests/unit/mailbox/test_rate_limiter_mailbox` |
| Phase 4 | `./build/tests/unit/mailbox/test_admission_policies` |
| Phase 5 | `./build/tests/unit/mailbox/test_admission_mailbox` |
| Phase 6 | `ctest -R "Metric" --output-on-failure && ./build/tests/unit/mailbox/test_rate_limiter_mailbox && ./build/tests/unit/mailbox/test_admission_mailbox` |
| Phase 7 | `./build/tests/unit/cli/test_actor_commands` |
| Phase 8 | `./build/tests/unit/config/test_rate_limiting_config` |
| Phase 9 | `ctest -R "RateLimiting|AdmissionPolicy|RateLimitingTopology" --output-on-failure -j4 --timeout 60` |
| Full | `ctest --output-on-failure -j4` (check no regressions) |

## 9. Acceptance Criteria

From the design spec, verified at phase completion:

### Rate Limiting

- [ ] Token bucket refills at the configured rate over a 5-second observation window (±2% tolerance). → Phase 2
- [ ] Burst allows a short spike above the sustained rate. → Phase 2
- [ ] After an idle period, the bucket refills to `burst` tokens. → Phase 2
- [ ] When rate limited, `dequeue()` returns `nullptr` while mailbox is non-empty. → Phase 3
- [ ] `MboxSnapshot` reports rate limiter state correctly. → Phase 3
- [ ] Metric `hpactor_actor_rate_limit_blocked_total` increments on each rate-limited dequeue. → Phase 6
- [ ] CLI `/actor <id> rate` shows enabled/disabled, rate, burst, tokens, blocked count. → Phase 7

### Admission Policy

- [ ] `TypeFilterPolicy` blocks messages with a specified TypeTag. → Phase 4
- [ ] `SenderFilterPolicy` blocks messages from a specified sender. → Phase 4
- [ ] `PriorityThresholdPolicy` rejects messages below minimum priority. → Phase 4
- [ ] `SizeLimitPolicy` rejects messages exceeding size threshold. → Phase 4
- [ ] Policy chain short-circuits on first rejection. → Phase 4
- [ ] `PerSenderRatePolicy` independently rate-limits each sender. → Phase 4
- [ ] `EnqueueResult` contains `FailureReason::RejectedByPolicy` when rejected. → Phase 5
- [ ] Metric `hpactor_actor_admission_rejected_total` increments on each rejection. → Phase 6
- [ ] CLI `/actor <id> admission` shows active policies and per-policy rejection counts. → Phase 7

### TOML Configuration

- [ ] `[system.rate_limiting]` sets system defaults. → Phase 8
- [ ] `[actor.rate_limiting]` overrides per actor. → Phase 8
- [ ] `[system.admission.*]` configures system-wide admission rules. → Phase 8
- [ ] `[actor.admission.*]` configures per-actor admission rules. → Phase 8
- [ ] Per-actor config wins over system defaults. → Phase 8

### Integration

- [ ] Fast producer + rate-limited actor: actual consumption rate matches configured rate. → Phase 9
- [ ] Admission policy rejects specific message types: confirmed via `EnqueueResult`. → Phase 9
- [ ] Per-sender isolation: one fast sender's rate limit does not affect other senders. → Phase 9
- [ ] Rate limiting + backpressure: high mailbox depth still triggers backpressure signals. → Phase 9 (no special handling needed — depth-based pressure works naturally)
- [ ] Rate limiting + overflow: rate-limited actor whose mailbox fills up correctly activates overflow handlers. → Phase 9

## 10. References

- [ACT-006 Design Spec](/Users/skg7on/Workspace/Projects/HPActor/.worktrees/act-006-rate-limiting/docs/superpowers/specs/2026-06-05-act006-rate-limiting-admission-design.md)
- [ACT-006 Issue](https://github.com/skg7on/HPActor/issues/11)
- [Architecture Requirement Backlog](../architecture/production/architecture-requirement-backlog.md)
- [Production Reliability Plane](../architecture/production/production-reliability-plane.md)
- [Mailbox Management & Backpressure Design](../architecture/actor/mailbox-management-backpressure-design.md)
- [Priority Mailbox Lanes Design](../architecture/production/priority-mailbox-lanes-design.md)
