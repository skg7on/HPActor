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
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>

namespace hpactor {

EventBasedActor::EventBasedActor(ActorContext* ctx, ActorSystem& sys)
    : LocalActor(ctx, sys) {}

void EventBasedActor::on_activate() {}

void EventBasedActor::receive(TypedMessage& msg) {
    if (!handlers_initialized_) {
        initialize_proto_handlers();
    }

    // Capture sender for reply() tracking
    auto* ctx = context();
    if (ctx) {
        ctx->set_current_sender(msg.sender_address());
    }

    // Try proto handler dispatch by TypeTag first
    auto it = proto_handlers_.find(msg.type_id());
    if (it != proto_handlers_.end()) {
        auto deserialized = it->second.deserialize(msg.payload());
        if (deserialized) {
            bytes response = it->second.invoke(std::move(deserialized));
            if (!response.empty() && ctx) {
                TypedMessage reply_msg(it->first, response);
                ctx->reply(std::move(reply_msg));
            }
        }
        return;
    }

    // Fall through to Behavior-based handling
    if (behavior_) {
        behavior_(msg);
    }
}

void EventBasedActor::become(Behavior bh) {
    behavior_ = std::move(bh);
}

void EventBasedActor::become_empty() {
    behavior_ = Behavior{};
}

void EventBasedActor::initialize_proto_handlers() {
    if (handlers_initialized_) return;
    register_handlers();
    handlers_initialized_ = true;
}

void EventBasedActor::on_proto_message(TypeTag tag, const bytes& payload) {
    if (!handlers_initialized_) {
        initialize_proto_handlers();
    }

    auto it = proto_handlers_.find(tag);
    if (it == proto_handlers_.end()) {
        return;
    }

    ProtoHandler& handler = it->second;
    auto msg = handler.deserialize(payload);
    if (!msg) return;

    bytes response = handler.invoke(std::move(msg));
    auto* ctx = context();
    if (!response.empty() && ctx) {
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

} // namespace hpactor
