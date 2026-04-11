#pragma once

#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/ref/actor_address.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// Actor - opaque handle to a local actor
// -----------------------------------------------------------------------------
class Actor {
  public:
    Actor() = default;

    explicit Actor(std::shared_ptr<AbstractActor> ptr)
        : actor_(std::move(ptr)) {}

    ActorId id() const {
        // TODO: delegate to actor_->id() once AbstractActor is defined
        return ActorId{};
    }

    ActorType type() const {
        // TODO: delegate to actor_->type() once AbstractActor is defined
        return ActorType{0};
    }

    ActorAddress address() const {
        // TODO: delegate to actor_->address() once AbstractActor is defined
        return ActorAddress{};
    }

    operator ActorAddress() const {
        return address();
    }

    explicit operator bool() const {
        return actor_ != nullptr;
    }

    void swap(Actor& other) noexcept {
        actor_.swap(other.actor_);
    }

  private:
    std::shared_ptr<AbstractActor> actor_;
};

} // namespace hpactor
