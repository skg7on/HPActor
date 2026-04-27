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

#include <functional>
#include <tuple>
#include <type_traits>
#include <variant>

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
template <typename Signature> class message_handler;

// -----------------------------------------------------------------------------
// handler_type - extracts result type from a typed message signature
// -----------------------------------------------------------------------------
template <typename T> struct handler_type;

template <typename R, typename Msg> struct handler_type<result<R>(Msg)> {
    using result = R;
    using message = Msg;

    template <typename F> result operator()(F&& f, Msg&& msg) {
        return f(std::move(msg));
    }
};

// -----------------------------------------------------------------------------
// TypedBehavior - statically typed behavior for typed actors
// -----------------------------------------------------------------------------
template <typename... Signatures> class TypedBehavior {
  public:
    using result_type = void;

    TypedBehavior() = default;

    template <typename T> TypedBehavior& operator()(T&& /*handler*/) {
        return *this;
    }

    result<void> invoke(TypedMessage& /*msg*/) {
        return result<void>::make();
    }

    bool matches(const TypedMessage& /*msg*/) const {
        return false;
    }

  private:
    std::tuple<message_handler<Signatures>...> handlers_;
};

// -----------------------------------------------------------------------------
// message_handler - handler for a single typed signature
// -----------------------------------------------------------------------------
template <typename R, typename Msg> class message_handler<result<R>(Msg)> {
  public:
    using signature = result<R>(Msg);

    message_handler() = default;

    template <typename F> explicit message_handler(F&& /*func*/) {}

    result<R> operator()(Msg&& /*msg*/) {
        return result<R>::make(error{});
    }

    bool matches(const TypedMessage& /*msg*/) const {
        return false;
    }
};

} // namespace hpactor