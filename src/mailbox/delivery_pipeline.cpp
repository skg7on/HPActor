// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_directory.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/adt/dedup_cache.hpp>
#include <hpactor/mailbox/backpressure_coordinator.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mailbox/delivery_pipeline.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>

#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/delivery_mode.hpp>
#include <hpactor/msg/failure_envelope.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/runtime/messaging_network_emitters.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>

namespace hpactor::mailbox {

using MetricBuf = metrics::MpscRingBuffer<metrics::MetricEvent>;

namespace {

bool local_signal_enabled(BackpressureMode mode) noexcept {
    return mode == BackpressureMode::LocalSignal ||
           mode == BackpressureMode::LocalAndRemoteSignal;
}

bool remote_signal_enabled(BackpressureMode mode) noexcept {
    return mode == BackpressureMode::RemoteSignal ||
           mode == BackpressureMode::LocalAndRemoteSignal;
}

bool pressure_result_should_signal(const EnqueueResult& result) noexcept {
    if (result.code == EnqueueResultCode::AcceptedWithSoftPressure) {
        return true;
    }
    return !result.accepted() && result.retryable();
}

} // namespace

// ── Static helpers ────────────────────────────────────────────────────────

bool DeliveryPipeline::should_dead_letter(DeadLetterQueue* dlq,
                                          DeliveryMode mode) noexcept {
    if (!dlq || !dlq->config().enabled) {
        return false;
    }
    return should_route_to_dlq(mode, dlq->config().routing_policy);
}

void DeliveryPipeline::set_dlq_trace_fields(DeadLetterRecord& dl,
                                            const TraceContext& tc) noexcept {
    uint64_t hi = 0, lo = 0, sp = 0;
    for (size_t i = 0; i < 8; ++i) {
        hi = (hi << 8) | tc.trace_id.bytes[i];
        lo = (lo << 8) | tc.trace_id.bytes[i + 8];
        sp = (sp << 8) | tc.span_id.bytes[i];
    }
    dl.trace_id_hi = hi;
    dl.trace_id_lo = lo;
    dl.span_id = sp;
}

// ── Construction ───────────────────────────────────────────────────────────

DeliveryPipeline::DeliveryPipeline(Config config)
    : config_(std::move(config)) {}

DeliveryPipeline::~DeliveryPipeline() = default;

// ── Pipeline stage: missing actor ──────────────────────────────────────────

EnqueueResult
DeliveryPipeline::reject_missing_actor(ActorId target, const TypedMessage& msg,
                                       const DeliveryOptions& options,
                                       uint8_t priority,
                                       int64_t deadline_ns) noexcept {
    if (should_dead_letter(config_.dlq, options.delivery_mode)) {
        DeadLetterRecord dl;
        dl.reason = DeadLetterReason::ActorNotFound;
        dl.source = DeadLetterSource::LocalDelivery;
        dl.sender = msg.sender_address();
        dl.target = ActorAddress{config_.endpoint, ActorType{0}, target, 0};
        dl.type_tag = msg.type_id();
        dl.message_id = options.message_id;
        dl.frame_flags = options.flags;
        dl.priority = priority;
        dl.deadline_ns = deadline_ns;
        dl.payload_sample = msg.payload();
        dl.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (msg.has_trace_context()) {
            set_dlq_trace_fields(dl, msg.trace_context());
        }
        (void)config_.dlq->try_push(std::move(dl));
    }

    EnqueueResult r;
    r.code = EnqueueResultCode::ActorNotFound;
    r.target = target;
    if (config_.metrics) {
        FailureEnvelope env = make_failure_envelope(
            FailureReason::NoRoute, target, msg.sender_address(),
            ActorAddress{config_.endpoint, ActorType{0}, target, 0},
            MessageId{options.message_id}, TraceContext{},
            FailureSource::ActorRuntime, "target actor not found in registry");
        metrics::MetricEvent evt{};
        evt.timestamp_ns = env.timestamp_ns;
        evt.actor_id = target;
        evt.event_type = metrics::MetricEventType::kDeliveryFailure;
        evt.code = static_cast<uint8_t>(env.reason);
        evt.value_hi = 1;
        config_.metrics->try_push(evt);
    }
    return r;
}

// ── Pipeline stage: circuit breaker DLQ ────────────────────────────────────

void DeliveryPipeline::push_circuit_open_dlq(ActorId target, const TypedMessage& msg,
                                             const DeliveryOptions& options,
                                             uint8_t priority, int64_t deadline_ns) {
    if (!should_dead_letter(config_.dlq, options.delivery_mode)) {
        return;
    }
    DeadLetterRecord dl;
    dl.reason = DeadLetterReason::EndpointCircuitOpen;
    dl.source = DeadLetterSource::LocalDelivery;
    dl.sender = msg.sender_address();
    dl.target = ActorAddress{config_.endpoint, ActorType{0}, target, 0};
    dl.type_tag = msg.type_id();
    dl.message_id = options.message_id;
    dl.frame_flags = options.flags;
    dl.priority = priority;
    dl.deadline_ns = deadline_ns;
    dl.payload_sample = msg.payload();
    dl.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (msg.has_trace_context()) {
        set_dlq_trace_fields(dl, msg.trace_context());
    }
    (void)config_.dlq->try_push(std::move(dl));
}

// ── Pipeline stage: circuit breaker admission gate ─────────────────────────

std::optional<EnqueueResult>
DeliveryPipeline::check_circuit_breaker(ActorId target, const TypedMessage& msg,
                                        const DeliveryOptions& options,
                                        uint8_t priority, int64_t deadline_ns) {
    if (!config_.actors) {
        return std::nullopt;
    }
    auto entry = config_.actors->find(target);
    auto actor_ptr = entry.has_value() ? entry->instance : nullptr;
    if (!actor_ptr || !actor_ptr->is_event_based_actor()) {
        return std::nullopt;
    }

    auto* eba = static_cast<EventBasedActor*>(actor_ptr.get());
    if (!eba->quarantine_enabled()) {
        return std::nullopt;
    }

    // System messages (tag < TypeTag::User) bypass the circuit breaker so
    // that CLI/admin commands (inspect, kill, quarantine/unquarantine) can
    // always reach the actor regardless of circuit state.
    if (is_system_message(msg.type_id())) {
        return std::nullopt;
    }

    auto* cb = eba->circuit_breaker();
    auto now = std::chrono::steady_clock::now();

    if (cb->state == CircuitBreakerState::kOpen) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - cb->opened_at);
        auto cooldown = eba->quarantine_policy().cooldown_period;
        if (elapsed >= cooldown) {
            cb->state = CircuitBreakerState::kHalfOpen;
            cb->half_open_probe_in_flight = true;
            if (auto* rb = eba->metrics_ring_buffer()) {
                auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  now.time_since_epoch())
                                  .count();
                metrics::MetricEvent evt{};
                evt.timestamp_ns = static_cast<uint64_t>(now_ns);
                evt.actor_id = target;
                evt.event_type = metrics::MetricEventType::kCircuitStateChange;
                evt.code = static_cast<uint8_t>(CircuitBreakerState::kHalfOpen);
                evt.value_hi = cb->trip_count;
                rb->try_push(evt);
            }
            return std::nullopt;
        }
        push_circuit_open_dlq(target, msg, options, priority, deadline_ns);
        return EnqueueResult{EnqueueResultCode::CircuitOpen, target};
    }
    if (cb->state == CircuitBreakerState::kHalfOpen) {
        if (cb->half_open_probe_in_flight) {
            push_circuit_open_dlq(target, msg, options, priority, deadline_ns);
            return EnqueueResult{EnqueueResultCode::CircuitOpen, target};
        }
        cb->half_open_probe_in_flight = true;
    }
    return std::nullopt;
}

// ── Pipeline stage: default TTL application ────────────────────────────────

int64_t DeliveryPipeline::apply_default_ttl(int64_t deadline_ns) const noexcept {
    if (deadline_ns != INT64_MAX || config_.default_message_ttl_ms.count() == 0) {
        return deadline_ns;
    }
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    uint64_t ttl_ns =
        static_cast<uint64_t>(config_.default_message_ttl_ms.count()) * 1'000'000ULL;
    if (ttl_ns > static_cast<uint64_t>(INT64_MAX) - now_ns) {
        return INT64_MAX - 1;
    }
    return static_cast<int64_t>(now_ns + ttl_ns);
}

// ── Pipeline stage: duplicate detection ────────────────────────────────────

std::optional<EnqueueResult>
DeliveryPipeline::check_duplicate(ActorId target, const TypedMessage& msg,
                                  const DeliveryOptions& options) {
    if (!is_tracked_delivery(options.delivery_mode) || !config_.dedup_cache ||
        options.message_id == 0) {
        return std::nullopt;
    }
    ActorId sender_id = msg.sender_address().id;
    if (!config_.dedup_cache->is_duplicate(config_.endpoint, sender_id,
                                           MessageId{options.message_id})) {
        return std::nullopt;
    }
    // ── Auto-ACK: duplicate detected → emit ACK(Duplicate) ──
    if (msg.ack_requested() && config_.reliable_ack) {
        ActorAddress acker{config_.endpoint, ActorType{0}, ActorId{0}, 0};
        (*config_.reliable_ack)(msg.sender_address(), acker, msg.message_id(),
                                static_cast<uint8_t>(2), // AckStatus::Duplicate
                                0);
    }

    if (config_.metrics) {
        uint64_t ts_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        metrics::MetricEvent evt{};
        evt.timestamp_ns = ts_ns;
        evt.actor_id = target;
        evt.event_type = metrics::MetricEventType::kDeliveryDuplicate;
        evt.code = static_cast<uint8_t>(FailureReason::Duplicate);
        evt.value_hi = 1;
        config_.metrics->try_push(evt);
    }
    return EnqueueResult{EnqueueResultCode::Accepted, target};
}

// ── Pipeline stage: deadline expiration check ──────────────────────────────

std::optional<EnqueueResult>
DeliveryPipeline::check_expired(ActorId target, const TypedMessage& msg,
                                const DeliveryOptions& options,
                                uint8_t priority, int64_t deadline_ns) {
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (!is_expired(deadline_ns, now_ns)) {
        return std::nullopt;
    }
    if (config_.metrics) {
        FailureEnvelope env = make_failure_envelope(
            FailureReason::Expired, target, msg.sender_address(),
            ActorAddress{config_.endpoint, ActorType{0}, target, 0},
            MessageId{options.message_id}, msg.trace_context(),
            FailureSource::ActorRuntime, "message deadline expired before enqueue");
        metrics::MetricEvent evt{};
        evt.timestamp_ns = env.timestamp_ns;
        evt.actor_id = target;
        evt.event_type = metrics::MetricEventType::kDeliveryExpired;
        evt.code = static_cast<uint8_t>(FailureReason::Expired);
        evt.value_hi = 1;
        config_.metrics->try_push(evt);
    }
    if (should_dead_letter(config_.dlq, options.delivery_mode)) {
        DeadLetterRecord dl;
        dl.reason = DeadLetterReason::Expired;
        dl.source = DeadLetterSource::LocalDelivery;
        dl.sender = msg.sender_address();
        dl.target = ActorAddress{config_.endpoint, ActorType{0}, target, 0};
        dl.type_tag = msg.type_id();
        dl.message_id = options.message_id;
        dl.frame_flags = options.flags;
        dl.priority = priority;
        dl.deadline_ns = deadline_ns;
        dl.payload_sample = msg.payload();
        dl.timestamp_ns = now_ns;
        if (msg.has_trace_context()) {
            set_dlq_trace_fields(dl, msg.trace_context());
        }
        (void)config_.dlq->try_push(std::move(dl));
    }
    return EnqueueResult{EnqueueResultCode::Rejected, target};
}

// ── Pipeline stage: rejection observability ────────────────────────────────

void DeliveryPipeline::emit_rejection_observability(
    ActorId target, const StreamBuffer& msg_payload,
    const TraceContext& msg_trace, bool msg_has_trace,
    const MailboxEnvelopeMeta& meta, const EnqueueResult& result,
    const DeliveryOptions& options, OverflowPolicy overflow_policy) {
    if (result.accepted()) {
        return;
    }

    if (should_dead_letter(config_.dlq, options.delivery_mode) &&
        overflow_policy == OverflowPolicy::DeadLetter) {
        DeadLetterRecord dl;
        dl.reason = DeadLetterReason::OverflowPolicy;
        dl.source = DeadLetterSource::MailboxAdmission;
        dl.sender = meta.sender;
        dl.target = ActorAddress{config_.endpoint, ActorType{0}, target, 0};
        dl.type_tag = meta.type_tag;
        dl.message_id = meta.message_id;
        dl.frame_flags = meta.flags;
        dl.priority = meta.priority;
        dl.deadline_ns = meta.deadline_ns;
        dl.mailbox_depth = result.depth;
        dl.mailbox_capacity = result.capacity;
        dl.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        dl.payload_sample = msg_payload;
        if (msg_has_trace) {
            set_dlq_trace_fields(dl, msg_trace);
        }
        (void)config_.dlq->try_push(std::move(dl));
    }

    if (config_.metrics) {
        FailureEnvelope env = make_failure_envelope(
            result.failure_reason(), target, meta.sender,
            ActorAddress{config_.endpoint, ActorType{0}, target, 0},
            MessageId{options.message_id}, TraceContext{},
            FailureSource::Mailbox, "mailbox admission rejected");
        metrics::MetricEvent evt{};
        evt.timestamp_ns = env.timestamp_ns;
        evt.actor_id = target;
        evt.event_type = metrics::MetricEventType::kDeliveryFailure;
        evt.code = static_cast<uint8_t>(env.reason);
        evt.value_hi = 1;
        config_.metrics->try_push(evt);
    }
}

// ── Pipeline stage: backpressure signal emission ───────────────────────────

void DeliveryPipeline::maybe_emit_backpressure(
    MPSCActorMailbox<TypedMessage>* mailbox, const EnqueueResult& result,
    const MailboxEnvelopeMeta& meta, bool emit_requested,
    BackpressureMode backpressure_mode, bool sender_is_remote) {
    if (!emit_requested || mailbox == nullptr ||
        !pressure_result_should_signal(result)) {
        return;
    }

    if (sender_is_remote && !remote_signal_enabled(backpressure_mode)) {
        return;
    }
    if (!sender_is_remote && !local_signal_enabled(backpressure_mode)) {
        return;
    }

    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const bool is_rejection = !result.accepted();
    auto sequence = mailbox->try_acquire_backpressure_signal(
        now_ns, result.pressure_state, is_rejection);
    if (!sequence.has_value()) {
        return;
    }

    BackpressureSignal signal;
    signal.target = ActorAddress{config_.endpoint, ActorType{0}, result.target, 0};
    signal.sender = meta.sender;
    signal.reason = result.pressure_reason;
    signal.depth = result.depth;
    signal.capacity = result.capacity;
    signal.bytes = result.bytes;
    signal.byte_capacity = result.byte_capacity;
    signal.pressure_ratio = result.pressure_ratio;
    signal.retry_after = result.retry_after;
    signal.sequence = sequence.value();

    if (config_.backpressure) {
        if (sender_is_remote) {
            config_.backpressure->emit_remote_signal(signal, result.pressure_state);
        } else {
            config_.backpressure->emit_local_signal(signal, result.pressure_state);
        }
    }
}

// ── Main entry point: try_deliver ──────────────────────────────────────────

EnqueueResult
DeliveryPipeline::try_deliver(ActorId target, TypedMessage msg, uint8_t priority,
                              int64_t deadline_ns, DeliveryOptions options) {
    auto* mailbox =
        config_.actors ? config_.actors->find_mailbox(target).get() : nullptr;
    if (mailbox == nullptr) {
        // Fallback: check for Disruptor-mailbox actor.
        if (config_.actors) {
            auto fixed_binding = config_.actors->find_fixed_binding(target);
            if (fixed_binding.has_value() && fixed_binding->valid()) {
                // Route system messages through the control port.
                if (is_system_message(msg.type_id())) {
                    return fixed_binding->control.try_push(
                        fixed_binding->control.context, std::move(msg));
                }
                // Route user-level TypedMessages through the dynamic lane
                // (Phase 7) when available; fall back to rejection.
                if (fixed_binding->control.try_push_dynamic) {
                    return fixed_binding->control.try_push_dynamic(
                        fixed_binding->control.context, std::move(msg));
                }
                EnqueueResult result;
                result.code = EnqueueResultCode::Rejected;
                result.target = target;
                return result;
            }
        }
        return reject_missing_actor(target, msg, options, priority, deadline_ns);
    }

    if (auto blocked =
            check_circuit_breaker(target, msg, options, priority, deadline_ns)) {
        return *blocked;
    }

    deadline_ns = apply_default_ttl(deadline_ns);
    msg.set_deadline_ns(deadline_ns);

    if (auto dup = check_duplicate(target, msg, options)) {
        return *dup;
    }

    if (auto expired = check_expired(target, msg, options, priority, deadline_ns)) {
        return *expired;
    }

    MailboxEnvelopeMeta meta;
    meta.sender = msg.sender_address();
    meta.type_tag = msg.type_id();
    meta.priority = priority;
    meta.deadline_ns = deadline_ns;
    meta.flags = options.flags;
    meta.message_id = options.message_id;
    if (options.no_drop) {
        meta.flags |= net::WireFrame::NoDrop;
    }
    meta.schedule_edf = options.schedule_edf;

    const auto bp_mode = mailbox->config().backpressure_mode;
    const bool sender_is_remote = meta.sender.endpoint != config_.endpoint &&
                                  meta.sender.id != ActorId{0};

    const StreamBuffer& msg_payload = msg.payload();
    TraceContext msg_trace;
    bool msg_has_trace = msg.has_trace_context();
    if (msg_has_trace) {
        msg_trace = msg.trace_context();
    }

    // ── Capture auto-ACK state before the message is moved into the mailbox ──
    const bool needs_auto_ack = msg.ack_requested();
    const uint64_t ack_msg_id = msg.message_id();
    const ActorAddress ack_sender = msg.sender_address();

    auto result = mailbox->try_push(std::move(msg), meta);

    if (!result.accepted()) {
        // ── Auto-ACK: admission rejected → emit NACK(Rejected) ──
        if (needs_auto_ack && config_.reliable_ack) {
            uint32_t retry_after = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(result.retry_after)
                    .count());
            if (retry_after == 0) {
                retry_after = 500; // default retry-after for NACK
            }
            ActorAddress acker{config_.endpoint, ActorType{0}, ActorId{0}, 0};
            (*config_.reliable_ack)(ack_sender, acker, ack_msg_id,
                                    static_cast<uint8_t>(1), // AckStatus::Rejected
                                    retry_after);
        }

        emit_rejection_observability(target, msg_payload, msg_trace,
                                     msg_has_trace, meta, result, options,
                                     mailbox->config().overflow_policy);
    }

    maybe_emit_backpressure(mailbox, result, meta, options.emit_backpressure,
                            bp_mode, sender_is_remote);

    return result;
}

// ── deliver_with_result ────────────────────────────────────────────────────

DeliveryResult
DeliveryPipeline::deliver_with_result(ActorId target, TypedMessage msg,
                                      uint8_t priority, int64_t deadline_ns,
                                      DeliveryOptions options) {
    auto er = try_deliver(target, std::move(msg), priority, deadline_ns, options);
    auto dr = DeliveryResult::from_enqueue(er, ActorAddress{}, {});
    if (config_.actors) {
        if (auto mbox = config_.actors->find_mailbox(target)) {
            mbox->record_delivery_result(dr.status);
        }
    }
    if (config_.metrics) {
        uint64_t ts_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        metrics::MetricEvent evt{};
        evt.timestamp_ns = ts_ns;
        evt.actor_id = target;
        evt.event_type = metrics::MetricEventType::kDeliveryResult;
        evt.code = static_cast<uint8_t>(dr.status);
        evt.value_hi = 1;
        config_.metrics->try_push(evt);
    }
    return dr;
}

} // namespace hpactor::mailbox
