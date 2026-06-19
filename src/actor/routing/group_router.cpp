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

#include <hpactor/actor/routing/group_router.hpp>

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_context.hpp>
#include <hpactor/adt/stream_buffer.hpp>

#include <algorithm>
#include <utility>

namespace hpactor::routing {

GroupRouter::GroupRouter(ActorContext* ctx, ActorSystem& sys,
                         std::unique_ptr<IRoutingLogic> logic,
                         std::string service_key)
    : EventBasedActor(ctx, sys), routing_logic_(std::move(logic)),
      service_key_(std::move(service_key)) {
    become(make_behavior());
}

Behavior GroupRouter::make_behavior() {
    return Behavior{[this](TypedMessage& msg) {
        if (routees_.empty()) {
            return; // no routees — drop the message
        }

        // Only collect snapshots when the strategy needs them.
        std::vector<cli::MboxSnapshot> snapshots;
        if (routing_logic_->needs_mailbox_snapshots()) {
            collect_snapshots(routees_, snapshots);
        }

        size_t idx = routing_logic_->select_routee(routees_, msg, snapshots);
        if (idx < routees_.size()) {
            context()->send(routees_[idx].address(), std::move(msg));
        }
    }};
}

void GroupRouter::add_routee(ActorRef routee) {
    routees_.emplace_back(std::move(routee));
    routing_logic_->on_routees_changed(routees_);
}

void GroupRouter::remove_routee(const ActorAddress& addr) {
    auto it = std::find_if(
        routees_.begin(), routees_.end(),
        [&addr](const ActorRef& ref) { return ref.address() == addr; });
    if (it != routees_.end()) {
        routees_.erase(it);
        routing_logic_->on_routees_changed(routees_);
    }
}

void GroupRouter::set_routees(std::vector<ActorRef> routees) {
    routees_ = std::move(routees);
    routing_logic_->on_routees_changed(routees_);
}

void GroupRouter::broadcast(TypedMessage msg) {
    broadcast_to_routees(context(), routees_, msg);
}

void GroupRouter::set_routing_logic(std::unique_ptr<IRoutingLogic> logic) {
    routing_logic_ = std::move(logic);
    routing_logic_->on_routees_changed(routees_);
}

} // namespace hpactor::routing
