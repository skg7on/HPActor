#pragma once

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_proxy.hpp>

#include <variant>

namespace hpactor {

// Forward declaration
class ActorSystem;

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

// -----------------------------------------------------------------------------
// ActorRef - unified reference to a local or remote actor
// -----------------------------------------------------------------------------
// ActorRef provides location-transparent access to actors. It can hold
// either a local Actor or a remote ActorProxy, and dispatches operations
// appropriately based on the underlying reference type.
// -----------------------------------------------------------------------------
class ActorRef {
public:
    // Default constructor creates an invalid reference
    ActorRef() = default;

    // Construct from a local actor
    ActorRef(Actor actor) : ref_(std::move(actor)) {}

    // Construct from a remote actor proxy
    ActorRef(ActorProxy proxy) : ref_(std::move(proxy)) {}

    // Get the actor's address
    ActorAddress address() const {
        if (is_local()) {
            return std::get<Actor>(ref_).address();
        } else {
            return std::get<ActorProxy>(ref_).address();
        }
    }

    // Check if this is a local actor
    bool is_local() const {
        return std::holds_alternative<Actor>(ref_);
    }

    // Check if this actor is valid
    explicit operator bool() const {
        if (is_local()) {
            return static_cast<bool>(std::get<Actor>(ref_));
        } else {
            return static_cast<bool>(std::get<ActorProxy>(ref_));
        }
    }

    // Get the node ID where this actor resides
    NodeId node_id() const {
        return address().node_id;
    }

    // Send a message to this actor
    // Note: For user-defined message types, use system().deliver_local() directly
    // until serialization is implemented in Phase 3
    void send(const ActorAddress& target, MessageVariant msg);

    // Access underlying Actor (for internal use)
    Actor* get_actor() {
        if (is_local()) {
            return &std::get<Actor>(ref_);
        }
        return nullptr;
    }

    // Access underlying ActorProxy (for internal use)
    ActorProxy* get_proxy() {
        if (!is_local()) {
            return &std::get<ActorProxy>(ref_);
        }
        return nullptr;
    }

private:
    std::variant<Actor, ActorProxy> ref_;
};

} // namespace hpactor
