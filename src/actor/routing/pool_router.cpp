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
      pool_size_(pool_size) {
    become(make_behavior());
}

void PoolRouter::on_activate() {
    // At this point the ActorContext is fully set up.
    // Spawn the initial routee children.
    spawn_routees(pool_size_);

    // Give consistent hashing a chance to build its ring.
    routing_logic_->on_routees_changed(routees_);
}

Behavior PoolRouter::make_behavior() {
    return Behavior{[this](TypedMessage& msg) {
        // DownMsg triggers child failure handling; do not forward to
        // routees. All other messages (including other system messages
        // already handled by dispatch_system_message) are forwarded.
        if (msg.type_id() == TypeTag::DownMsg) {
            SelfSupervisingActor::handle_child_down(msg.type_id(), msg.payload());
            return;
        }

        if (routees_.empty()) {
            return; // no routees — drop the message
        }

        // Only collect snapshots when the strategy needs them (avoids
        // per-message vector allocation for RoundRobin/Random/ConsHash).
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

void PoolRouter::spawn_routees(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        // Inline the factory→spawn_configured→add_child logic without
        // triggering on_routees_changed on every iteration — we notify
        // once after the loop.
        auto actor_ptr = factory_(context(), home_system());
        config::ActorDef def;
        def.behavior = std::string(actor_ptr->type_name());
        auto child = home_system().spawn_configured(std::move(actor_ptr), def);
        SelfSupervisingActor::add_child(child);
        routees_.emplace_back(std::move(child));
    }
    routing_logic_->on_routees_changed(routees_);
}

void PoolRouter::add_routee() {
    // Create the actor instance via the factory, then register it with the
    // actor system via spawn_configured() so it gets a mailbox, context,
    // scheduler registration, etc.
    auto actor_ptr = factory_(context(), home_system());

    config::ActorDef def;
    def.behavior = std::string(actor_ptr->type_name());

    auto child = home_system().spawn_configured(std::move(actor_ptr), def);
    SelfSupervisingActor::add_child(child);
    routees_.emplace_back(std::move(child));
    routing_logic_->on_routees_changed(routees_);
}

void PoolRouter::remove_routee() {
    if (routees_.empty()) {
        return;
    }

    ActorRef last = routees_.back();

    // Remove from supervision tracking.
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
    if (new_size == routees_.size()) {
        return;
    }

    if (new_size > routees_.size()) {
        size_t diff = new_size - routees_.size();
        spawn_routees(diff);
    } else {
        size_t diff = routees_.size() - new_size;
        for (size_t i = 0; i < diff; ++i) {
            remove_routee();
        }
    }

    pool_size_ = new_size;
}

void PoolRouter::broadcast(TypedMessage msg) {
    broadcast_to_routees(context(), routees_, msg);
}

void PoolRouter::set_routing_logic(std::unique_ptr<IRoutingLogic> logic) {
    routing_logic_ = std::move(logic);
    routing_logic_->on_routees_changed(routees_);
}

SupervisionDirective PoolRouter::on_failure(ActorId child_id, const error& err) {
    // Let the base class decide the directive based on restart policy.
    auto directive = SelfSupervisingActor::on_failure(child_id, err);

    if (directive != SupervisionDirective::Restart) {
        // Remove the failed routee from our list.
        auto it = std::find_if(routees_.begin(), routees_.end(),
                               [child_id](const ActorRef& ref) {
                                   return ref.address().id == child_id;
                               });
        if (it != routees_.end()) {
            routees_.erase(it);
            routing_logic_->on_routees_changed(routees_);
        }
        return directive;
    }

    // Find the failed routee and replace it.
    auto it = std::find_if(routees_.begin(), routees_.end(),
                           [child_id](const ActorRef& ref) {
                               return ref.address().id == child_id;
                           });
    if (it == routees_.end()) {
        return SupervisionDirective::Stop;
    }

    // Spawn a replacement and swap it in.
    auto new_actor_ptr = factory_(context(), home_system());
    config::ActorDef def;
    def.behavior = std::string(new_actor_ptr->type_name());
    auto new_child = home_system().spawn_configured(std::move(new_actor_ptr), def);
    SelfSupervisingActor::add_child(new_child);
    *it = ActorRef(std::move(new_child));

    routing_logic_->on_routees_changed(routees_);
    return SupervisionDirective::Restart;
}

} // namespace hpactor::routing
