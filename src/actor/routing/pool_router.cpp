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

#include <hpactor/actor/routing/pool_router.hpp>

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_context.hpp>
#include <hpactor/config/topology_model.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>

#include <algorithm>
#include <utility>

namespace hpactor::routing {

PoolRouter::PoolRouter(ActorContext* ctx, ActorSystem& sys,
                       std::unique_ptr<IRoutingLogic> logic,
                       config::ActorFactory factory, size_t pool_size,
                       SupervisionPolicy policy)
    : SelfSupervisingActor(ctx, sys, std::move(policy)),
      routing_logic_(std::move(logic)), factory_(std::move(factory)),
      needs_snapshots_(routing_logic_->needs_mailbox_snapshots()),
      initial_pool_size_(pool_size) {
    become(make_behavior());
}

void PoolRouter::on_activate() {
    spawn_routees(initial_pool_size_);
    routing_logic_->on_routees_changed(routees_);
}

Behavior PoolRouter::make_behavior() {
    return Behavior{[this](TypedMessage& msg) {
        // DownMsg triggers child failure handling; do not forward.
        if (msg.type_id() == TypeTag::DownMsg) {
            SelfSupervisingActor::handle_child_down(msg.type_id(), msg.payload());
            return;
        }

        if (routees_.empty()) {
            return;
        }

        std::vector<cli::MboxSnapshot> snapshots;
        if (needs_snapshots_) {
            collect_snapshots(routees_, snapshots);
        }

        size_t idx = routing_logic_->select_routee(routees_, msg, snapshots);
        if (idx < routees_.size()) {
            context()->send(routees_[idx].address(), std::move(msg));
        }
    }};
}

Actor PoolRouter::spawn_single_routee() {
    auto actor_ptr = factory_(context(), home_system());
    config::ActorDef def;
    def.behavior = std::string(actor_ptr->type_name());
    auto child = home_system().spawn_configured(std::move(actor_ptr), def);
    SelfSupervisingActor::add_child(child);
    return child;
}

void PoolRouter::spawn_routees(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        routees_.emplace_back(spawn_single_routee());
    }
    routing_logic_->on_routees_changed(routees_);
}

void PoolRouter::add_routee() {
    routees_.emplace_back(spawn_single_routee());
    routing_logic_->on_routees_changed(routees_);
}

void PoolRouter::remove_routee() {
    if (routees_.empty()) {
        return;
    }

    ActorRef last = routees_.back();
    if (last.is_local()) {
        auto* actor = last.get_actor();
        if (actor && actor->get()) {
            SelfSupervisingActor::remove_child(*actor);
        }
    }

    routees_.pop_back();
    routing_logic_->on_routees_changed(routees_);
}

void PoolRouter::resize(size_t new_size) {
    if (new_size > routees_.size()) {
        spawn_routees(new_size - routees_.size());
    } else {
        while (routees_.size() > new_size) {
            remove_routee();
        }
    }
}

void PoolRouter::broadcast(TypedMessage msg) {
    broadcast_to_routees(context(), routees_, msg);
}

void PoolRouter::set_routing_logic(std::unique_ptr<IRoutingLogic> logic) {
    needs_snapshots_ = logic->needs_mailbox_snapshots();
    routing_logic_ = std::move(logic);
    routing_logic_->on_routees_changed(routees_);
}

SupervisionDirective PoolRouter::on_failure(ActorId child_id, const error& err) {
    auto directive = SelfSupervisingActor::on_failure(child_id, err);

    auto it = std::find_if(routees_.begin(), routees_.end(),
                           [child_id](const ActorRef& ref) {
                               return ref.address().id == child_id;
                           });
    if (it == routees_.end()) {
        return directive;
    }

    if (directive != SupervisionDirective::Restart) {
        routees_.erase(it);
        routing_logic_->on_routees_changed(routees_);
        return directive;
    }

    // Restart: replace the failed routee with a fresh spawn.
    *it = ActorRef(spawn_single_routee());
    routing_logic_->on_routees_changed(routees_);
    return SupervisionDirective::Restart;
}

} // namespace hpactor::routing
