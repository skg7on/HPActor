// src/actor/event_based_actor.cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>

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
    // Clean up coroutine if still running
    if (actor_coroutine_ && !actor_coroutine_.done()) {
        // Force termination
        actor_coroutine_.task().handle().destroy();
        actor_coroutine_ = sched::ActorCoroutine{};
    }
}

} // namespace hpactor