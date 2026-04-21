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

#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>

namespace hpactor {

ActorContext::ActorContext(Actor owner, ActorSystem* system)
    : owner_(std::move(owner)), system_(system) {}

ActorContext::~ActorContext() = default;

void ActorContext::send(const ActorAddress& target, MessageVariant msg) {
    if (target.is_local()) {
        auto actor_ptr = owner_.get();
        if (actor_ptr) {
            actor_ptr->system().deliver_local(target.id, std::move(msg));
        } else if (system_) {
            // Fallback: use system directly when owner is not set
            system_->deliver_local(target.id, std::move(msg));
        }
    } else {
        // Remote delivery - will be implemented in Phase 1
        // For now, ignore remote sends
    }
}

void ActorContext::send_with_priority(const ActorAddress& target, MessageVariant msg,
                                    uint8_t priority, int64_t deadline_ns) {
    if (target.is_local()) {
        auto actor_ptr = owner_.get();
        if (!actor_ptr) {
            return;
        }
        actor_ptr->system().deliver_local(target.id, std::move(msg), priority, deadline_ns);
    } else {
        // Remote delivery - will be implemented in Phase 1
        // For now, ignore remote sends
    }
}

void ActorContext::reply(MessageVariant msg) {
    // TODO: reply to the sender of the current message
    (void)msg;
}

void ActorContext::reply_with_error(error err) {
    // TODO: reply with error to the sender of the current message
    (void)err;
}

void ActorContext::schedule(std::chrono::milliseconds delay, MessageVariant msg) {
    // TODO: schedule message via actor system's clock/alarm mechanism
    (void)delay;
    (void)msg;
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
    monitored_.push_back(target);
}

RpcFuture<bytes> ActorContext::rpc(const ActorAddress& target,
                                   const bytes& encoded_request,
                                   std::chrono::milliseconds timeout_ms) {
    return system_->rpc_channel().call_raw(target, encoded_request, timeout_ms);
}

} // namespace hpactor