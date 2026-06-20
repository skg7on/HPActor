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
#include <hpactor/actor/ask_manager.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/receptionist/receptionist.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/tracing/trace_manager.hpp>

#include <google/protobuf/message.h>

#include <chrono>
#include <thread>

namespace hpactor {

ActorContext::ActorContext(Actor owner, ActorSystem* system)
    : owner_(std::move(owner)), system_(system) {}

ActorContext::~ActorContext() = default;

ActorContext::TraceScope::TraceScope(ActorContext* ctx, const TraceContext& next) noexcept
    : ctx_(ctx) {
    if (ctx_ == nullptr) {
        return;
    }
    previous_ = ctx_->current_trace_context_;
    previous_valid_ = ctx_->has_current_trace_context_;
    ctx_->set_current_trace_context(next);
}

ActorContext::TraceScope::~TraceScope() {
    if (ctx_ == nullptr) {
        return;
    }
    if (previous_valid_) {
        ctx_->set_current_trace_context(previous_);
    } else {
        ctx_->clear_current_trace_context();
    }
}

ActorRef ActorContext::resolve(const ActorAddress& target) {
    // 1. Check cache (hot path)
    auto cached = ref_cache_.get(target.id);
    if (cached.has_value()) {
        return *cached;
    }

    // 2. Resolve system pointer
    // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator)
    auto* system = system_ != nullptr
                       ? system_
                       : (owner_ ? &owner_.get()->system() : nullptr);
    if (system == nullptr) {
        return ActorRef{};
    }

    // 3. If target endpoint differs from our own, it's a remote actor.
    //    Skip local lookup — actor IDs are only unique within a system.
    if (!(target.endpoint == system->endpoint())) {
        ActorProxy proxy(target, system);
        ActorRef ref(proxy);
        // Only cache if transport was resolved successfully
        if (ref.get_proxy() != nullptr && ref.get_proxy()->transport() != nullptr) {
            ref_cache_.put(target.id, ref);
        }
        return ref;
    }

    // 4. Local endpoint: check local actors
    auto actor = system->get_actor(target.id);
    if (actor != nullptr) {
        ActorRef ref{Actor(actor)};
        ref_cache_.put(target.id, ref);
        return ref;
    }

    return ActorRef{};
}

void ActorContext::send(ActorRef& target, TypedMessage msg) {
    // Stamp sender identity for reply tracking
    if (owner_) {
        msg.set_sender_address(owner_.address());
    }

    auto* system = system_ != nullptr
                       ? system_
                       : (owner_ ? &owner_.get()->system() : nullptr);
    if (system != nullptr && system->trace_manager() != nullptr) {
        system->trace_manager()->inject_message_context(
            msg, this,
            system->trace_manager()->config().create_roots_for_actor_context_sends);
    }

    target.send(target.address(), std::move(msg));
}

void ActorContext::send(const ActorAddress& target, TypedMessage msg) {
    (void)try_send(target, std::move(msg));
}

void ActorContext::send(const ActorAddress& target, TypeTag tag,
                        const google::protobuf::Message& proto_msg) {
    TypedMessage msg(tag, proto_msg);
    send(target, std::move(msg));
}

void ActorContext::send_with_priority(const ActorAddress& target, TypedMessage msg,
                                      uint8_t priority, int64_t deadline_ns) {
    (void)try_send_with_priority(target, std::move(msg), priority, deadline_ns);
}

msg::DeliveryReceipt
ActorContext::try_send(const ActorAddress& target, TypedMessage msg,
                       mailbox::DeliveryOptions options) {
    auto ref = resolve(target);
    if (!ref) {
        return msg::DeliveryReceipt(
            mailbox::DeliveryResult{mailbox::DeliveryStatus::NoRoute, target,
                                    MessageId{options.message_id}, 0});
    }

    if (owner_) {
        msg.set_sender_address(owner_.address());
    }

    auto* system = owner_ ? &owner_.get()->system() : system_;
    if (system != nullptr && system->trace_manager() != nullptr) {
        system->trace_manager()->inject_message_context(
            msg, this,
            system->trace_manager()->config().create_roots_for_actor_context_sends);
    }

    return msg::DeliveryReceipt(ref.try_send(ref.address(), std::move(msg), options));
}

msg::DeliveryReceipt
ActorContext::try_send_with_priority(const ActorAddress& target, TypedMessage msg,
                                     uint8_t priority, int64_t deadline_ns,
                                     mailbox::DeliveryOptions options) {
    auto ref = resolve(target);
    if (!ref) {
        return msg::DeliveryReceipt(
            mailbox::DeliveryResult{mailbox::DeliveryStatus::NoRoute, target,
                                    MessageId{options.message_id}, 0});
    }

    if (owner_) {
        msg.set_sender_address(owner_.address());
    }

    auto* system = owner_ ? &owner_.get()->system() : system_;
    if (system != nullptr && system->trace_manager() != nullptr) {
        system->trace_manager()->inject_message_context(
            msg, this,
            system->trace_manager()->config().create_roots_for_actor_context_sends);
    }

    if (ref.is_local()) {
        if (system != nullptr) {
            auto er = system->try_deliver_local(target.id, std::move(msg),
                                                priority, deadline_ns, options);
            auto dr = mailbox::DeliveryResult::from_enqueue(
                er, target, MessageId{options.message_id});
            return msg::DeliveryReceipt(std::move(dr));
        }
        return msg::DeliveryReceipt(
            mailbox::DeliveryResult{mailbox::DeliveryStatus::NoRoute, target,
                                    MessageId{options.message_id}, 0});
    }

    return msg::DeliveryReceipt(ref.try_send(ref.address(), std::move(msg), options));
}

void ActorContext::send_edf(ActorAddress target, TypedMessage msg,
                            std::chrono::nanoseconds deadline, uint8_t priority) {
    auto* sys = system_;
    if (!sys)
        return;

    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    int64_t deadline_ns = now_ns + deadline.count();
    if (deadline_ns < now_ns)
        deadline_ns = now_ns + 1; // clamp overflow

    msg.set_sender_address(owner_.address());
    sys->deliver_local_edf(target.id, std::move(msg), deadline_ns, priority);
}

void ActorContext::reply(TypedMessage msg) {
    // If replying to a tracked ask, route through AskManager
    if (current_ask_message_id_ != 0 && system_ && system_->ask_manager()) {
        StreamBuffer payload = msg.payload();
        system_->ask_manager()->on_response(current_ask_message_id_,
                                            std::move(payload));
        current_ask_message_id_ = 0;
        return;
    }
    if (current_sender_.id != ActorId{0}) {
        send(current_sender_, std::move(msg));
    }
}

msg::DeliveryReceipt
ActorContext::try_reply(TypedMessage msg, mailbox::DeliveryOptions options) {
    if (current_sender_.id == ActorId{0}) {
        return msg::DeliveryReceipt(mailbox::DeliveryResult{
            mailbox::DeliveryStatus::NoRoute, ActorAddress{},
            MessageId{options.message_id}, 0});
    }
    return try_send(current_sender_, std::move(msg), options);
}

void ActorContext::reply(TypeTag tag, const google::protobuf::Message& proto_msg) {
    TypedMessage msg(tag, proto_msg);
    reply(std::move(msg));
}

void ActorContext::reply_with_error(const error& err) {
    if (current_sender_.id == ActorId{0}) {
        return;
    }

    // Wire format: [4 bytes: error code BE][error message string]
    // A protobuf error message can replace this payload later without
    // changing the TypeTag or dispatch path.
    StreamBuffer payload;
    const uint32_t code = err.code();
    payload.push_back(static_cast<uint8_t>((code >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((code >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code & 0xFF));
    const auto& msg = err.message();
    payload.insert(payload.end(), msg.begin(), msg.end());

    TypedMessage error_msg(TypeTag::ErrorMsg, std::move(payload));
    send(current_sender_, std::move(error_msg));
}

AlarmHandle
ActorContext::schedule(std::chrono::milliseconds delay, TypedMessage msg) {
    auto* sched = system_->scheduler();
    if (!sched)
        return AlarmHandle{};

    msg.set_sender_address(owner_.address());

    ActorId self_id = owner_.id();
    ActorSystem* sys = system_;

    // Wrap in shared_ptr because TypedMessage is move-only and std::function
    // requires a copyable callable.
    auto msg_ptr = std::make_shared<TypedMessage>(std::move(msg));
    auto callback = [sys, self_id, msg_ptr]() {
        sys->deliver_local(self_id, std::move(*msg_ptr));
    };

    int64_t delay_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
    auto handle = sched->schedule_after(std::move(callback), delay_ns);
    return AlarmHandle{handle.value()};
}

void ActorContext::cancel_schedule(AlarmHandle handle) {
    if (handle.value() == 0)
        return;
    auto* sched = system_->scheduler();
    if (!sched)
        return;
    sched->cancel_timer(sched::TimerHandle{handle.value()});
}

void ActorContext::passivate() {
    // Self-passivation: the actor's lifecycle is transitioned to
    // kPassivating after the current handler returns. The
    // PassivationManager (owned by ActorSystem) handles the protocol.
    // This method sets a flag that the actor runner checks after
    // the current activation completes.
    passivation_requested_ = true;
}

void ActorContext::receptionist_register(receptionist::ServiceKey key) {
    auto* sys = owner_ ? &owner_.get()->system() : system_;
    if (!sys)
        return;
    auto* rec = sys->receptionist();
    if (!rec)
        return;
    rec->register_actor(key, owner_.address());
}

void ActorContext::receptionist_unregister(receptionist::ServiceKey key) {
    auto* sys = owner_ ? &owner_.get()->system() : system_;
    if (!sys)
        return;
    auto* rec = sys->receptionist();
    if (!rec)
        return;
    rec->unregister_actor(key, owner_.address());
}

void ActorContext::receptionist_subscribe(receptionist::ServiceKey key) {
    auto* sys = owner_ ? &owner_.get()->system() : system_;
    if (!sys)
        return;
    auto* rec = sys->receptionist();
    if (!rec)
        return;
    rec->add_subscriber(key, owner_.address());
}

void ActorContext::receptionist_unsubscribe(receptionist::ServiceKey key) {
    auto* sys = owner_ ? &owner_.get()->system() : system_;
    if (!sys)
        return;
    auto* rec = sys->receptionist();
    if (!rec)
        return;
    rec->remove_subscriber(key, owner_.address());
}

ActorRef ActorContext::register_message_adapter(
    std::function<TypedMessage(const TypedMessage&)> fn, TypeTag from_tag) {
    if (!owner_)
        return ActorRef{};
    auto* raw = owner_.get().get();
    if (!raw || !raw->is_event_based_actor())
        return ActorRef{};
    auto* eba = static_cast<EventBasedActor*>(raw);
    eba->add_message_adapter(std::move(fn), from_tag);
    // Return a self-ref — adapted messages arrive at this actor.
    return ActorRef(owner_);
}

std::vector<Actor> ActorContext::children() const {
    return children_;
}

void ActorContext::add_child(Actor child) {
    children_.push_back(std::move(child));
}

void ActorContext::remove_child(Actor child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->address() == child.address()) {
            children_.erase(it);
            return;
        }
    }
}

std::vector<ActorRef> ActorContext::remote_children() const {
    return remote_children_;
}

void ActorContext::add_remote_child(ActorRef child) {
    remote_children_.push_back(std::move(child));
}

std::vector<ActorAddress> ActorContext::linked_actors() const {
    return linked_;
}

void ActorContext::monitor(const ActorAddress& target) {
    add_monitored(target);
}

RpcFuture<StreamBuffer> ActorContext::rpc(const ActorAddress& target,
                                          const StreamBuffer& encoded_request,
                                          std::chrono::milliseconds timeout_ms) {
    const TraceContext* trace =
        has_current_trace_context() ? &current_trace_context() : nullptr;
    return system_->rpc_channel().call_raw(target, encoded_request, timeout_ms,
                                           trace);
}

RequestHandle<StreamBuffer>
ActorContext::ask_raw(const ActorAddress& target,
                      const StreamBuffer& encoded_request, RequestTimeout timeout) {
    if (!system_) {
        RequestHandle<StreamBuffer> h;
        h.resolve_error(error(errors::unknown, "no system"));
        return h;
    }

    ActorRef ref = resolve(target);

    if (ref.is_local()) {
        ActorId requester_id = owner_ ? owner_.address().id : ActorId{0};
        auto reg = system_->ask_manager()->register_ask(
            requester_id, target, timeout, system_->config().default_ask_timeout_ms);

        TypedMessage msg(TypeTag::Invalid, encoded_request);
        msg.set_ask_message_id(reg.msg_id.value());
        send(target, std::move(msg));

        return std::move(reg.handle);
    }

    // Remote: not yet bridged through ask(); return error handle
    RequestHandle<StreamBuffer> h;
    h.resolve_error(error(errors::unknown, "remote ask not yet supported via "
                                           "ask_raw"));
    return h;
}

RpcFuture<StreamBuffer>
ActorContext::http_get(const std::string& url, std::vector<net::HttpHeader> headers) {
    return system_->http_client().get(url, std::move(headers));
}

RpcFuture<StreamBuffer>
ActorContext::http_post(const std::string& url, StreamBuffer body,
                        std::vector<net::HttpHeader> headers) {
    return system_->http_client().post(url, std::move(body), std::move(headers));
}

RpcFuture<StreamBuffer>
ActorContext::http_put(const std::string& url, StreamBuffer body,
                       std::vector<net::HttpHeader> headers) {
    return system_->http_client().put(url, std::move(body), std::move(headers));
}

RpcFuture<StreamBuffer>
ActorContext::http_delete(const std::string& url,
                          std::vector<net::HttpHeader> headers) {
    return system_->http_client().del(url, std::move(headers));
}

RpcFuture<StreamBuffer>
ActorContext::http_request(net::HttpMethod method, const std::string& url,
                           std::vector<net::HttpHeader> headers, StreamBuffer body) {
    return system_->http_client().request(method, url, std::move(headers),
                                          std::move(body));
}

void ActorContext::on_backpressure(BackpressureHandler handler) {
    backpressure_handler_ = std::move(handler);
}

void ActorContext::handle_backpressure(const mailbox::BackpressureSignal& signal) {
    if (backpressure_handler_) {
        backpressure_handler_(signal);
    }
}

void ActorContext::stop(ActorId target) {
    // Resolve system pointer
    auto* system = system_ != nullptr
                       ? system_
                       : (owner_ ? &owner_.get()->system() : nullptr);
    if (system == nullptr) {
        return;
    }

    // Resolve target actor
    auto actor = system->get_actor(target);
    if (actor == nullptr) {
        return;
    }

    // Emit drain start metric event
    if (auto* ring_buf = system->metrics_ring_buffer()) {
        metrics::MetricEvent evt{};
        evt.actor_id = target;
        evt.event_type = metrics::MetricEventType::kActorDrainStart;
        evt.value_hi = 1;
        ring_buf->try_push(evt);
    }

    // Check for lifecycle support
    auto* lc = actor->as_lifecycle();
    if (lc == nullptr) {
        // No lifecycle: call on_exit directly if EventBasedActor
        if (actor->is_event_based_actor()) {
            auto* eba = static_cast<EventBasedActor*>(actor.get());
            eba->on_exit();
        }
        return;
    }

    auto policy = lc->drain_config().policy;

    if (policy == DrainPolicy::ImmediateStop) {
        // Drain mailbox directly (dead-letter all messages)
        if (actor->is_event_based_actor()) {
            auto* eba = static_cast<EventBasedActor*>(actor.get());
            eba->drain_all_immediate();
        }
        // Drive lifecycle: kActive -> kStopping -> kStopped
        lc->transition(LifecycleState::kStopping);
        lc->transition(LifecycleState::kStopped);
        // Notify linked/monitored actors
        if (actor->is_event_based_actor()) {
            auto* eba = static_cast<EventBasedActor*>(actor.get());
            eba->on_exit();
        }
    } else {
        // Transition to kDraining (invokes on_drain() hook)
        lc->transition(LifecycleState::kDraining);
        // Start drain timer (completes drain on timeout or when mailbox
        // empties)
        if (actor->is_event_based_actor()) {
            auto* eba = static_cast<EventBasedActor*>(actor.get());
            eba->start_drain_timer();
        } else {
            // Non-EventBasedActor with lifecycle but no drain timer support:
            // transition directly to stopped.
            lc->transition(LifecycleState::kStopping);
            lc->transition(LifecycleState::kStopped);
        }
    }
}

result<void>
ActorContext::stop_sync(ActorId target, std::chrono::milliseconds timeout) {
    stop(target);

    // Resolve system pointer for polling
    auto* system = system_ != nullptr
                       ? system_
                       : (owner_ ? &owner_.get()->system() : nullptr);
    if (system == nullptr) {
        return result<void>::make(error(errors::actor_not_found, "no actor "
                                                                 "system "
                                                                 "available"));
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        auto actor = system->get_actor(target);
        if (actor == nullptr) {
            // Actor removed from registry (already fully stopped / cleaned up)
            return result<void>::make();
        }

        auto* lc = actor->as_lifecycle();
        if (lc != nullptr && lc->state() == LifecycleState::kStopped) {
            return result<void>::make();
        }
        if (lc == nullptr) {
            // Non-lifecycle actors: on_exit was called synchronously in stop(),
            // consider it done.
            return result<void>::make();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return result<void>::make(error(errors::timeout, "stop_sync timed out"));
}

} // namespace hpactor
