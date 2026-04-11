#include <hpactor/actor_context.hpp>

namespace hpactor {

ActorContext::ActorContext(Actor owner) : owner_(std::move(owner)) {}

ActorContext::~ActorContext() = default;

void ActorContext::send(const ActorAddress& target, MessageVariant msg) {
    // TODO: delegate to actor system's enqueue mechanism
    (void)target;
    (void)msg;
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

std::vector<ActorAddress> ActorContext::linked_actors() const {
    return linked_;
}

void ActorContext::monitor(const ActorAddress& target) {
    monitored_.push_back(target);
}

} // namespace hpactor