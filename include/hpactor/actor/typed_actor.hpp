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
#include <hpactor/typed_behavior.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// TypedEventBasedActor - statically typed event-based actor
// -----------------------------------------------------------------------------
template <typename... Signatures>
class TypedEventBasedActor : public LocalActor {
  public:
    using behavior_type = TypedBehavior<Signatures...>;

    void become(behavior_type bh) {
        behavior_ = std::move(bh);
    }

    template <typename T>
    result<typename matching_handler_type<std::decay_t<T>,
                                          Signatures...>::result>
    operator()(T&& msg) {
        return behavior_.template invoke_msg<T>(std::forward<T>(msg));
    }

  protected:
    virtual behavior_type make_behavior() = 0;

    TypedEventBasedActor(ActorContext* ctx, ActorSystem& sys)
        : LocalActor(ctx, sys) {}

    TypedEventBasedActor(ActorId id, ActorContext* ctx, ActorSystem& sys)
        : LocalActor(id, ctx, sys) {}

    void on_activate() override {}
    void on_deactivate() override {}

    void receive(TypedMessage& /*msg*/) override {}

  private:
    behavior_type behavior_;
};

// -----------------------------------------------------------------------------
// typed_actor - type-safe reference to a TypedEventBasedActor
// -----------------------------------------------------------------------------
template <typename... Signatures> class typed_actor {
  public:
    using base_type = TypedEventBasedActor<Signatures...>;

    typed_actor() = default;
    explicit typed_actor(std::shared_ptr<base_type> ptr)
        : actor_(std::move(ptr)) {}

    template <typename T> void operator()(T&& msg) {
        if (actor_) {
            (*actor_)(std::forward<T>(msg));
        }
    }

    ActorId id() const {
        if (actor_) {
            return actor_->id();
        }
        return ActorId{};
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

  private:
    std::shared_ptr<base_type> actor_;
};

} // namespace hpactor