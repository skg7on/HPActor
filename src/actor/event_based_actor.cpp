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

void EventBasedActor::on_activate() {
    // Scheduler and mailbox are set by ActorSystem on spawn
}

void EventBasedActor::receive(MessageVariant&& msg) {
    // Legacy non-coroutine receive path (for actors not using act())
    if (behavior_) {
        behavior_(std::move(msg));
    }
}

void EventBasedActor::become(Behavior bh) {
    behavior_ = std::move(bh);
}

void EventBasedActor::become_empty() {
    behavior_ = Behavior{};
}

void EventBasedActor::on_deactivate() {
#if HPACTOR_USE_COROUTINES
    // Clean up coroutine if still running
    if (actor_coroutine_ && !actor_coroutine_.done()) {
        // Force termination
        actor_coroutine_.task().handle().destroy();
        actor_coroutine_ = sched::ActorCoroutine{};
    }
#endif
}

} // namespace hpactor
