#include <hpactor/actor/event_based_actor.hpp>

namespace hpactor {

EventBasedActor::EventBasedActor(ActorContext* ctx, ActorSystem& sys)
    : LocalActor(ctx, sys) {}

void EventBasedActor::become(Behavior bh) {
    behavior_ = std::move(bh);
}

void EventBasedActor::become_empty() {
    behavior_ = Behavior{};
}

void EventBasedActor::receive(MessageVariant&& msg) {
    if (behavior_) {
        behavior_(std::move(msg));
    }
}

void EventBasedActor::on_activate() {
    // Default implementation: set initial behavior
    behavior_ = make_behavior();
}

void EventBasedActor::on_deactivate() {
    // Default implementation: clear behavior
    behavior_ = Behavior{};
}

} // namespace hpactor
