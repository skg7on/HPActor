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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/quarantine_reason.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/messages.pb.h>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/tracing/trace_manager.hpp>
#include <hpactor/types/failure_envelope.hpp>

#include <chrono>
#include <cstring>
#include <memory>

namespace hpactor {

namespace {

class ReceiveSpanGuard {
  public:
    ReceiveSpanGuard(tracing::TraceManager* manager, tracing::SpanHandle* handle) noexcept
        : manager_(manager), handle_(handle) {}

    ~ReceiveSpanGuard() {
        if (manager_ != nullptr && handle_ != nullptr) {
            manager_->finish_span(*handle_, status_);
        }
    }

    void set_status(tracing::SpanStatus status) noexcept {
        status_ = status;
    }

  private:
    tracing::TraceManager* manager_{nullptr};
    tracing::SpanHandle* handle_{nullptr};
    tracing::SpanStatus status_{tracing::SpanStatus::kOk};
};

} // namespace

EventBasedActor::EventBasedActor(ActorContext* ctx, ActorSystem& sys)
    : LocalActor(ctx, sys) {}

void EventBasedActor::configure_quarantine(const QuarantinePolicy& policy) {
    quarantine_policy_ = policy;
    if (policy.enabled) {
        auto window_ms = static_cast<uint32_t>(policy.observation_window.count());
        failure_rate_tracker_.bucket_interval_ms =
            window_ms / FailureRateTracker::kNumBuckets;
        if (failure_rate_tracker_.bucket_interval_ms == 0) {
            failure_rate_tracker_.bucket_interval_ms = 1;
        }
    }
}

void EventBasedActor::on_activate() {}

void EventBasedActor::receive(TypedMessage& msg) {
    auto* ctx = context();
    auto* trace_manager = system().trace_manager();
    tracing::SpanHandle receive_span;
    std::unique_ptr<ActorContext::TraceScope> trace_scope;

    if (trace_manager != nullptr && trace_manager->enabled() &&
        trace_manager->config().record_actor_receive_spans) {
        tracing::SpanStart start;
        start.name = "hpactor.actor.receive";
        start.kind = tracing::SpanKind::kConsumer;
        start.has_parent = msg.has_trace_context();
        if (msg.has_trace_context()) {
            start.parent = msg.trace_context();
        }
        start.actor_id = id();
        start.sender_actor_id = msg.sender_address().id;
        start.type_tag = msg.type_id();
        start.payload_size = static_cast<uint32_t>(msg.payload().size());
        receive_span = trace_manager->start_span(start);
        if (ctx != nullptr && receive_span.context.valid()) {
            trace_scope = std::make_unique<ActorContext::TraceScope>(
                ctx, receive_span.context);
        }
    }

    ReceiveSpanGuard span_guard(trace_manager, &receive_span);
    // -- Drain gate: apply drain policy to every message during kDraining --
    if (auto* lc = as_lifecycle()) {
        if (lc->state() == LifecycleState::kDraining) {
            if (!drain_one(msg)) {
                // Drain-completion check: if mailbox is now empty, finish drain
                if (mailbox_is_empty()) {
                    cancel_drain_timer();
                    lc->transition(LifecycleState::kStopping);
                    lc->transition(LifecycleState::kStopped);
                    on_exit();
                }
                return; // message was dead-lettered by the drain policy
            }
        }
    }
    // -- End drain gate --

    // -- System message interception (link / monitor / death) --
    {
        bool handled = false;
        switch (msg.type_id()) {
            case TypeTag::LinkMsg:
                handled = handle_link_msg(msg);
                break;
            case TypeTag::UnlinkMsg:
                handled = handle_unlink_msg(msg);
                break;
            case TypeTag::MonitorMsg:
                handled = handle_monitor_msg(msg);
                break;
            case TypeTag::DemonitorMsg:
                handled = handle_demonitor_msg(msg);
                break;
            case TypeTag::DownMsg:
                handle_down_msg(msg);
                break;
            default:
                break;
        }
        if (handled)
            return;
    }
    // -- End system message interception --

    // -- Lifecycle message gate --
    // User messages (TypeTag >= 0x1000) are only accepted in ACTIVE state
    // or while draining (the drain gate handles drain policy decisions).
    // System messages (TypeTag < 0x1000) always pass through.
    if (static_cast<uint32_t>(msg.type_id()) >= 0x1000) {
        if (auto* lc = as_lifecycle()) {
            if (!lc->accepts_user_msgs() &&
                lc->state() != LifecycleState::kDraining) {
                // Quarantine-specific: build FailureEnvelope and emit metrics.
                if (lc->is_quarantined()) {
                    if (metrics_ring_buffer_) [[unlikely]] {
                        FailureEnvelope env = make_failure_envelope(
                            FailureReason::Quarantined, id(), msg.sender_address(),
                            ActorAddress{}, MessageId{0}, TraceContext{},
                            FailureSource::ActorRuntime, "actor is quarantined");
                        metrics::MetricEvent evt{};
                        evt.timestamp_ns = env.timestamp_ns;
                        evt.actor_id = id();
                        evt.event_type = metrics::MetricEventType::kDeliveryFailure;
                        evt.code = static_cast<uint8_t>(FailureReason::Quarantined);
                        evt.value_hi = 1;
                        metrics_ring_buffer_->try_push(evt);
                    }
                    if (logger_) [[unlikely]] {
                        HPACTOR_LOG_WARNING(
                            log::LogCategory::kActor, id(), 0, "quarantine_reject",
                            log::field_lit("reason",
                                           to_string(lc->quarantine_reason())));
                    }
                }
                return;
            }
        }
    }
    // -- End lifecycle message gate --

    if (!handlers_initialized_) {
        initialize_proto_handlers();
    }

    // Capture sender for reply() tracking
    ctx = context();
    if (ctx != nullptr) {
        ctx->set_current_sender(msg.sender_address());
    }

    // -- CLI introspection dispatch --
    {
        // InspectStateRequest: gather metadata + optional
        // state/mailbox/children
        if (msg.type_id() == TypeTag::InspectStateRequestTag) {
            cli::InspectStateRequest req;
            if (!req.ParseFromArray(msg.payload().data(),
                                    static_cast<int>(msg.payload().size()))) {
                return;
            }

            cli::InspectStateReply reply;
            auto meta = to_metadata();
            auto* pb_meta = reply.mutable_metadata();
            pb_meta->set_actor_id(meta.actor_id);
            pb_meta->set_actor_type(meta.actor_type);
            pb_meta->set_state(meta.state);
            pb_meta->set_incarnation(meta.incarnation);

            if (req.include_mailbox()) {
                auto ms = mailbox_snapshot();
                auto* pb_mbox = reply.mutable_mailbox();
                pb_mbox->set_depth(ms.depth);
                pb_mbox->set_total_enqueued(ms.total_enqueued);
                pb_mbox->set_total_dequeued(ms.total_dequeued);
                pb_mbox->set_max_depth(ms.max_depth);
                pb_mbox->set_high_priority_depth(ms.high_priority_depth);
                pb_mbox->set_capacity(ms.capacity);
                pb_mbox->set_queued_bytes(ms.queued_bytes);
                pb_mbox->set_byte_capacity(ms.byte_capacity);
                pb_mbox->set_pressure_ratio_ppm(ms.pressure_ratio_ppm);
                pb_mbox->set_total_rejected(ms.total_rejected);
                pb_mbox->set_total_dropped(ms.total_dropped);
                pb_mbox->set_total_dead_letters(ms.total_dead_letters);
                pb_mbox->set_pressure_state(ms.pressure_state);
                pb_mbox->set_overflow_policy(ms.overflow_policy);
            }

            if (req.include_state()) {
                auto blob = serialize_state();
                reply.set_state_blob(std::string(
                    reinterpret_cast<const char*>(blob.data()), blob.size()));
            }

            std::string reply_data = reply.SerializeAsString();
            StreamBuffer payload(reply_data.begin(), reply_data.end());
            ctx->reply(TypedMessage(TypeTag::InspectStateResponseTag,
                                    std::move(payload)));
            return;
        }

        // KillRequest: terminate this actor
        if (msg.type_id() == TypeTag::KillRequestTag) {
            cli::KillRequest req;
            if (!req.ParseFromArray(msg.payload().data(),
                                    static_cast<int>(msg.payload().size()))) {
                return;
            }

            cli::KillReply reply;
            reply.set_success(true);
            reply.set_error_code(0);

            std::string reply_data = reply.SerializeAsString();
            StreamBuffer payload(reply_data.begin(), reply_data.end());
            ctx->reply(TypedMessage(TypeTag::KillResponseTag, std::move(payload)));

            // Drive lifecycle state machine for graceful stop
            if (auto* lc = as_lifecycle()) {
                lc->transition(LifecycleState::kStopping);
                lc->transition(LifecycleState::kStopped);
            }
            set_exit_reason(0);
            return;
        }
    }
    // -- End CLI dispatch --

    auto t0 = metrics_ring_buffer_ ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};

    // Try proto handler dispatch by TypeTag first
    auto it = proto_handlers_.find(msg.type_id());
    if (it != proto_handlers_.end()) {
        auto deserialized = it->second.deserialize(msg.payload());
        if (deserialized) {
            StreamBuffer response = it->second.invoke(std::move(deserialized));
            if (!response.empty() && ctx != nullptr) {
                TypedMessage reply_msg(it->first, response);
                ctx->reply(std::move(reply_msg));
            }
            if (quarantine_policy_.enabled) [[unlikely]] {
                record_circuit_breaker_result(true);
            }
        } else {
            if (quarantine_policy_.enabled) [[unlikely]] {
                record_circuit_breaker_result(false);
            }
        }
        if (metrics_ring_buffer_) [[unlikely]] {
            auto t1 = std::chrono::steady_clock::now();
            auto ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            metrics::MetricEvent evt{};
            evt.actor_id = id();
            evt.event_type = metrics::MetricEventType::kMessageProcessed;
            evt.value_hi = static_cast<uint32_t>(ns > UINT32_MAX ? UINT32_MAX : ns);
            metrics_ring_buffer_->try_push(evt);
        }
        return;
    }

    // Fall through to Behavior-based handling
    if (behavior_) {
        behavior_(msg);
    }

    if (quarantine_policy_.enabled) [[unlikely]] {
        record_circuit_breaker_result(true);
    }

    if (metrics_ring_buffer_) [[unlikely]] {
        auto t1 = std::chrono::steady_clock::now();
        auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        metrics::MetricEvent evt{};
        evt.actor_id = id();
        evt.event_type = metrics::MetricEventType::kMessageProcessed;
        evt.value_hi = static_cast<uint32_t>(ns > UINT32_MAX ? UINT32_MAX : ns);
        metrics_ring_buffer_->try_push(evt);
    }

    // -- Drain-completion check after processing a message --
    if (auto* lc = as_lifecycle()) {
        if (lc->state() == LifecycleState::kDraining && mailbox_is_empty()) {
            cancel_drain_timer();
            lc->transition(LifecycleState::kStopping);
            lc->transition(LifecycleState::kStopped);
            on_exit();
        }
    }
    // -- End drain-completion check --
}

bool EventBasedActor::handle_link_msg(const TypedMessage& msg) {
    auto* ctx = context();
    if (ctx == nullptr)
        return true;
    const auto& sender = msg.sender_address();
    bool already_linked = false;
    for (const auto& linked : ctx->linked_actors()) {
        if (linked == sender) {
            already_linked = true;
            break;
        }
    }
    if (!already_linked) {
        ctx->add_linked(sender);
    }
    return true;
}

bool EventBasedActor::handle_unlink_msg(const TypedMessage& msg) {
    auto* ctx = context();
    if (ctx != nullptr) {
        ctx->remove_linked(msg.sender_address());
    }
    return true;
}

bool EventBasedActor::handle_monitor_msg(const TypedMessage& msg) {
    auto* ctx = context();
    if (ctx == nullptr)
        return true;
    const auto& sender = msg.sender_address();
    bool already = false;
    for (const auto& m : ctx->monitored_actors()) {
        if (m == sender) {
            already = true;
            break;
        }
    }
    if (!already) {
        ctx->add_monitored(sender);
    }
    return true;
}

bool EventBasedActor::handle_demonitor_msg(const TypedMessage& msg) {
    auto* ctx = context();
    if (ctx != nullptr) {
        ctx->remove_monitored(msg.sender_address());
    }
    return true;
}

void EventBasedActor::handle_down_msg(const TypedMessage& msg) {
    auto* ctx = context();
    if (ctx != nullptr) {
        ctx->remove_linked(msg.sender_address());
        ctx->remove_monitored(msg.sender_address());
    }
}

void EventBasedActor::become(Behavior bh) {
    behavior_ = std::move(bh);
}

void EventBasedActor::become_empty() {
    behavior_ = Behavior{};
}

void EventBasedActor::initialize_proto_handlers() {
    if (handlers_initialized_)
        return;
    register_handlers();
    handlers_initialized_ = true;
}

void EventBasedActor::on_proto_message(TypeTag tag, const StreamBuffer& payload) {
    if (!handlers_initialized_) {
        initialize_proto_handlers();
    }

    auto it = proto_handlers_.find(tag);
    if (it == proto_handlers_.end()) {
        return;
    }

    ProtoHandler& handler = it->second;
    auto msg = handler.deserialize(payload);
    if (!msg)
        return;

    StreamBuffer response = handler.invoke(std::move(msg));
    auto* ctx = context();
    if (!response.empty() && ctx != nullptr) {
        TypedMessage reply_msg(tag, response);
        ctx->reply(std::move(reply_msg));
    }
}

void EventBasedActor::on_deactivate() {
#if HPACTOR_SUPPORT_COROUTINES
    if (actor_coroutine_ && !actor_coroutine_.done()) {
        actor_coroutine_.task().handle().destroy();
        actor_coroutine_ = sched::ActorCoroutine{};
    }
#endif
}

void EventBasedActor::on_exit() {
    auto* ctx = context();
    if (ctx == nullptr) {
        return;
    }

    HPACTOR_LOG_INFO(log::LogCategory::kActor, id(),
                     static_cast<uint32_t>(log::LogEventId::kActorTerminated),
                     "actor terminated");

    if (metrics_ring_buffer_) [[unlikely]] {
        metrics::MetricEvent evt{};
        evt.actor_id = id();
        evt.event_type = metrics::MetricEventType::kActorTerminated;
        evt.value_hi = static_cast<uint32_t>(exit_reason_);
        metrics_ring_buffer_->try_push(evt);
    }

    // Build DownMessage
    hpactor::DownMessage pb;
    pb.set_actor_id(id().value());
    pb.set_reason_code(exit_reason_);

    StreamBuffer payload(pb.ByteSizeLong());
    (void)pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    // Send DownMsg to all linked actors
    for (const auto& addr : ctx->linked_actors()) {
        TypedMessage down_msg(TypeTag::DownMsg, StreamBuffer(payload));
        ctx->send(addr, std::move(down_msg));
    }

    // Send DownMsg to all monitored actors
    for (const auto& addr : ctx->monitored_actors()) {
        TypedMessage down_msg(TypeTag::DownMsg, StreamBuffer(payload));
        ctx->send(addr, std::move(down_msg));
    }
    // linked_ and monitored_ vectors will be destroyed with the context
}

cli::MboxSnapshot EventBasedActor::mailbox_snapshot() const {
    if (mailbox_)
        return mailbox_->snapshot();
    return {};
}

// ── Drain helpers
// ─────────────────────────────────────────────────────────────

bool EventBasedActor::drain_one(TypedMessage& msg) {
    auto* lc = as_lifecycle();
    if (!lc)
        return true; // no lifecycle = process normally

    auto policy = lc->drain_config().policy;
    bool is_system = static_cast<uint32_t>(msg.type_id()) < 0x1000;

    switch (policy) {
        case DrainPolicy::Drain:
            return true; // process normally

        case DrainPolicy::DropUserMessages:
            if (!is_system) {
                mailbox::DeadLetterRecord record;
                record.reason = mailbox::DeadLetterReason::DrainPolicyDrop;
                record.source = mailbox::DeadLetterSource::MailboxAdmission;
                record.sender = msg.sender_address();
                record.target = address();
                record.type_tag = msg.type_id();
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              system().clock().now().time_since_epoch())
                              .count();
                record.timestamp_ns = static_cast<uint64_t>(ns);
                system().dead_letter(std::move(record));
                return false;
            }
            return true;

        case DrainPolicy::ImmediateStop:
            return false; // handled at drain trigger, not per-message

        case DrainPolicy::SnapshotAndStop:
        case DrainPolicy::TransferShard: {
            // Deferred — fall back to Drain (log warning)
            HPACTOR_LOG_WARNING(
                log::LogCategory::kActor, id(),
                static_cast<uint32_t>(log::LogEventId::kActorTerminated),
                "deferred drain policy — falling back to Drain");
            lc->set_drain_config(
                DrainConfig{DrainPolicy::Drain, lc->drain_config().timeout});
            return true;
        }
    }
    return true;
}

void EventBasedActor::drain_all_immediate() {
    TypedMessage msg;
    while (mailbox_ && mailbox_->try_pop(msg)) {
        mailbox::DeadLetterRecord record;
        record.reason = mailbox::DeadLetterReason::MailboxClosed;
        record.source = mailbox::DeadLetterSource::MailboxAdmission;
        record.sender = msg.sender_address();
        record.target = address();
        record.type_tag = msg.type_id();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      system().clock().now().time_since_epoch())
                      .count();
        record.timestamp_ns = static_cast<uint64_t>(ns);
        system().dead_letter(std::move(record));
    }
}

void EventBasedActor::start_drain_timer() {
    auto* lc = as_lifecycle();
    if (!lc || !scheduler_)
        return;

    auto timeout = lc->drain_config().timeout;
    auto delay_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
    std::weak_ptr<AbstractActor> weak_self = shared_from_this();
    drain_timer_handle_ = scheduler_->schedule_after(
        [weak_self]() {
            if (auto self = weak_self.lock()) {
                auto* actor_ptr = static_cast<EventBasedActor*>(self.get());
                if (auto* lc2 = actor_ptr->as_lifecycle()) {
                    if (lc2->state() == LifecycleState::kDraining) {
                        lc2->on_drain_timeout();
                        // Dead-letter remaining mailbox messages
                        actor_ptr->drain_all_immediate();
                        lc2->transition(LifecycleState::kStopping);
                        lc2->transition(LifecycleState::kStopped);
                        actor_ptr->on_exit();
                    }
                }
            }
        },
        delay_ns);
}

void EventBasedActor::cancel_drain_timer() {
    if (drain_timer_handle_.valid() && scheduler_) {
        scheduler_->cancel_timer(drain_timer_handle_);
        drain_timer_handle_ = sched::TimerHandle{};
    }
}

void EventBasedActor::record_circuit_breaker_result(bool success) {
    if (!quarantine_policy_.enabled)
        return;

    if (success) {
        if (circuit_breaker_.state == CircuitBreakerState::kHalfOpen) {
            circuit_breaker_.state = CircuitBreakerState::kClosed;
            circuit_breaker_.trip_count = 0;
            circuit_breaker_.half_open_probe_in_flight = false;
        }
        return;
    }

    double threshold =
        static_cast<double>(quarantine_policy_.failure_rate_threshold);
    if (threshold <= 0.0)
        return;

    auto now = std::chrono::steady_clock::now();
    failure_rate_tracker_.advance_buckets(now);
    failure_rate_tracker_.record_failure();
    auto window_ms =
        static_cast<uint32_t>(quarantine_policy_.observation_window.count());

    static constexpr double kAlpha = 2.0 / 11.0;
    circuit_breaker_.failure_ema =
        kAlpha * failure_rate_tracker_.failure_rate(window_ms) +
        (1.0 - kAlpha) * circuit_breaker_.failure_ema;

    if (circuit_breaker_.failure_ema <= threshold)
        return;

    if (circuit_breaker_.state == CircuitBreakerState::kClosed ||
        circuit_breaker_.state == CircuitBreakerState::kHalfOpen) {
        circuit_breaker_.state = CircuitBreakerState::kOpen;
        circuit_breaker_.opened_at = now;
        circuit_breaker_.half_open_probe_in_flight = false;
        circuit_breaker_.trip_count++;
    }

    auto max_trips = quarantine_policy_.max_circuit_trips;
    if (max_trips > 0 && circuit_breaker_.trip_count >= max_trips) {
        if (auto* lc = as_lifecycle()) {
            lc->transition_to_quarantined(QuarantineReason::CircuitBreakerTrip);
        }
    }
}

} // namespace hpactor
