#pragma once

#include <hpactor/actor/abstract_actor.hpp>
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
        if (actor_) {
            return actor_->id();
        }
        return ActorId{};
    }

    ActorType type() const {
        if (actor_) {
            return actor_->type();
        }
        return ActorType{0};
    }

    ActorAddress address() const {
        if (actor_) {
            return actor_->address();
        }
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

    // Access the underlying actor (for internal use)
    std::shared_ptr<AbstractActor> get() const { return actor_; }

  private:
    std::shared_ptr<AbstractActor> actor_;
};

} // namespace hpactor
