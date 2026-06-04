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

#pragma once

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_proxy.hpp>

#include <variant>

namespace hpactor {

// Forward declaration
class ActorSystem;

/// \brief Opaque handle to a local actor.
///
/// Wraps a \c shared_ptr<AbstractActor>. Lightweight to copy; the underlying
/// actor is reference-counted. A default-constructed \c Actor is empty and
/// evaluates to \c false.
class Actor {
  public:
    /// \brief Construct an empty (invalid) handle.
    Actor() = default;

    /// \brief Construct a handle from an actor pointer.
    /// \param[in] ptr Shared pointer to an \c AbstractActor.
    explicit Actor(std::shared_ptr<AbstractActor> ptr)
        : actor_(std::move(ptr)) {}

    /// \brief Globally-unique actor identifier.
    ///
    /// Returns \c ActorId{} when the handle is empty.
    ActorId id() const {
        if (actor_) {
            return actor_->id();
        }
        return ActorId{};
    }

    /// \brief Actor type tag.
    ActorType type() const {
        if (actor_) {
            return actor_->type();
        }
        return ActorType{0};
    }

    /// \brief Full network-addressable identity.
    ActorAddress address() const {
        if (actor_) {
            return actor_->address();
        }
        return ActorAddress{};
    }

    /// \brief Implicit conversion to \c ActorAddress.
    operator ActorAddress() const {
        return address();
    }

    /// \brief Returns \c true if the handle is non-empty.
    explicit operator bool() const {
        return actor_ != nullptr;
    }

    void swap(Actor& other) noexcept {
        actor_.swap(other.actor_);
    }

    /// \brief Access the underlying \c AbstractActor pointer (internal use).
    std::shared_ptr<AbstractActor> get() const {
        return actor_;
    }

  private:
    std::shared_ptr<AbstractActor> actor_;
};

/// \brief Unified, location-transparent reference to an actor.
///
/// Holds either a local \c Actor or a remote \c ActorProxy in a variant.
/// All send operations dispatch to the correct path transparently.
///
/// \note Thread safety: \c send() delegates to the underlying actor or
///       proxy which synchronize internally.
class ActorRef {
  public:
    /// \brief Construct an invalid reference.
    ActorRef() = default;

    /// \brief Construct from a local actor.
    ActorRef(Actor actor) : ref_(std::move(actor)) {}

    /// \brief Construct from a remote actor proxy.
    ActorRef(ActorProxy proxy) : ref_(std::move(proxy)) {}

    /// \brief Get the actor's address (local or remote).
    ActorAddress address() const {
        if (is_local()) {
            return std::get<Actor>(ref_).address();
        } else {
            return std::get<ActorProxy>(ref_).address();
        }
    }

    /// \brief Returns \c true if this reference is to a local actor.
    bool is_local() const {
        return std::holds_alternative<Actor>(ref_);
    }

    /// \brief Returns \c true if the reference is valid (local or remote).
    explicit operator bool() const {
        if (is_local()) {
            return static_cast<bool>(std::get<Actor>(ref_));
        } else {
            return static_cast<bool>(std::get<ActorProxy>(ref_));
        }
    }

    /// \brief Network endpoint where this actor resides.
    EndPoint endpoint() const {
        return address().endpoint;
    }

    /// \brief Send a message to this actor.
    ///
    /// For local actors, delivers directly to the mailbox. For remote actors,
    /// serializes and sends over the transport.
    /// \param[in] target Destination address (usually the actor's own).
    /// \param[in] msg Message to send (moved).
    void send(const ActorAddress& target, TypedMessage msg);

    /// \brief Try-send returning a unified delivery result.
    ///
    /// For local actors, delegates to \c ActorSystem::try_deliver_local()
    /// and maps the result. For remote actors, delegates to
    /// \c ActorProxy::try_send().
    ///
    /// \param[in] target Destination address.
    /// \param[in] msg Message to send.
    /// \param[in] options Delivery options (deadline, priority, idempotency).
    /// \return \c DeliveryResult describing the delivery outcome.
    mailbox::DeliveryResult try_send(const ActorAddress& target, TypedMessage msg,
                                     mailbox::DeliveryOptions options = {});

    /// \brief Access underlying \c Actor (internal use).
    ///
    /// Returns \c nullptr if this is a remote reference.
    Actor* get_actor() {
        if (is_local()) {
            return &std::get<Actor>(ref_);
        }
        return nullptr;
    }

    /// \brief Access underlying \c ActorProxy (internal use).
    ///
    /// Returns \c nullptr if this is a local reference.
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
