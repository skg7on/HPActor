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

#include <memory>

#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor/typed_behavior.hpp>

namespace hpactor {

/// \brief Statically typed event-based actor.
///
/// Instead of a dynamic \c Behavior that accepts \c TypedMessage, this
/// actor uses a \c TypedBehavior<Signatures...> that dispatches based on
/// compile-time message signatures. Provides stronger type safety and
/// eliminates the need for runtime type-tag dispatch.
///
/// \tparam Signatures Message type signatures this actor handles.
///
/// \note Thread safety: All message handling executes on the actor's
///       scheduler worker thread.
template <typename... Signatures>
class TypedEventBasedActor : public LocalActor {
  public:
    using behavior_type = TypedBehavior<Signatures...>;

    /// \brief Replace the current behavior.
    ///
    /// \param[in] bh New typed behavior to install.
    void become(behavior_type bh) {
        behavior_ = std::move(bh);
    }

    /// \brief Invoke the typed behavior with a message.
    ///
    /// \tparam T Message type to dispatch.
    /// \param[in,out] msg Message forwarded to the matching handler.
    /// \return The result of the typed handler invocation.
    template <typename T>
    result<typename matching_handler_type<std::decay_t<T>, Signatures...>::result>
    operator()(T&& msg) {
        return behavior_.template invoke_msg<T>(std::forward<T>(msg));
    }

    void on_activate() override {}
    void on_deactivate() override {}

  protected:
    /// \brief Override to return the actor's initial typed behavior.
    virtual behavior_type make_behavior() = 0;

    /// \brief Construct with a context and system reference.
    TypedEventBasedActor(ActorContext* ctx, ActorSystem& sys)
        : LocalActor(ctx, sys) {}

    /// \brief Construct with an explicit ID, context, and system reference.
    TypedEventBasedActor(ActorId id, ActorContext* ctx, ActorSystem& sys)
        : LocalActor(id, ctx, sys) {}

    void receive(TypedMessage& /*msg*/) override {}

  private:
    behavior_type behavior_;
};

/// \brief Type-safe reference handle to a \c TypedEventBasedActor.
///
/// Provides compile-time checked dispatch — the handle statically
/// constrains which message types can be sent. Used instead of a
/// generic \c ActorRef when the caller wants type safety at the
/// send site.
///
/// \tparam Signatures Message type signatures matching the target actor.
template <typename... Signatures> class TypedEventBasedActorRef {
  public:
    using base_type = TypedEventBasedActor<Signatures...>;

    /// \brief Construct an empty (invalid) handle.
    TypedEventBasedActorRef() = default;

    /// \brief Construct from a shared pointer to the actor.
    /// \param[in] ptr Shared pointer to a \c TypedEventBasedActor.
    explicit TypedEventBasedActorRef(std::shared_ptr<base_type> ptr)
        : actor_(std::move(ptr)) {}

    /// \brief Forward a message to the typed actor.
    ///
    /// No-op if the handle is empty.
    /// \tparam T Message type (must match one of the actor's Signatures).
    /// \param[in,out] msg Message forwarded to the actor.
    template <typename T> void operator()(T&& msg) {
        if (actor_) {
            (*actor_)(std::forward<T>(msg));
        }
    }

    /// \brief Actor ID, or \c ActorId{} if empty.
    ActorId id() const {
        if (actor_) {
            return actor_->id();
        }
        return ActorId{};
    }

    /// \brief Actor address, or \c ActorAddress{} if empty.
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

  private:
    std::shared_ptr<base_type> actor_;
};

} // namespace hpactor
