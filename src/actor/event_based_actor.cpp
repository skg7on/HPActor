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
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/messages.pb.h>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/tracing/trace_manager.hpp>

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

void EventBasedActor::on_activate() {}

void EventBasedActor::receive(TypedMessage& msg) {
    auto* trace_manager = system().trace_manager();
    tracing::SpanHandle receive_span;
    std::unique_ptr<ActorContext::TraceScope> trace_scope;

    auto* ctx = context();
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
    // -- System message interception (link / monitor / death) --
    {
        switch (msg.type_id()) {
            case TypeTag::LinkMsg: {
                // Bidirectional link handshake: add sender to our linked_ set
                if (ctx != nullptr) {
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
                }
                return; // System message — fully handled, do not forward
            }

            case TypeTag::UnlinkMsg: {
                if (ctx != nullptr) {
                    ctx->remove_linked(msg.sender_address());
                }
                return; // System message — fully handled
            }

            case TypeTag::MonitorMsg: {
                // Add sender to our monitored_ list (one-way relationship).
                // The sender wants to be notified when we die.
                if (ctx != nullptr) {
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
                }
                return; // System message — fully handled
            }

            case TypeTag::DemonitorMsg: {
                // Remove sender from our monitored_ list.
                if (ctx != nullptr) {
                    ctx->remove_monitored(msg.sender_address());
                }
                return; // System message — fully handled
            }

            case TypeTag::DownMsg: {
                // Clean up linked/monitored entries for the dead actor
                if (ctx != nullptr) {
                    ctx->remove_linked(msg.sender_address());
                    ctx->remove_monitored(msg.sender_address());
                }
                // Fall through — behavior/supervision must see DownMsg
                break;
            }

            default:
                break;
        }
    }
    // -- End system message interception --

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

            // Schedule termination with normal exit code
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

} // namespace hpactor
