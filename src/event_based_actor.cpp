#include <hpactor/actor/event_based_actor.hpp>

namespace hpactor {

event_based_actor::event_based_actor(ActorContext* ctx, ActorSystem& sys)
    : local_actor(ctx, sys) {}

void event_based_actor::become(Behavior bh) {
    behavior_ = std::move(bh);
}

void event_based_actor::become_empty() {
    behavior_ = Behavior{};
}

void event_based_actor::receive(MessageVariant&& msg) {
    if (behavior_) {
        behavior_(std::move(msg));
    }
}

void event_based_actor::on_activate() {
    // Default implementation: set initial behavior
    behavior_ = make_behavior();
}

void event_based_actor::on_deactivate() {
    // Default implementation: clear behavior
    behavior_ = Behavior{};
}

} // namespace hpactor
