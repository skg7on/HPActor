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
// lambda_traits — extract argument/result type from a callable
// -----------------------------------------------------------------------------
template <typename F> struct lambda_traits;

template <typename C, typename R, typename Arg>
struct lambda_traits<R (C::*)(Arg)> {
    using argument_type = Arg;
    using result_type = R;
};

template <typename C, typename R, typename Arg>
struct lambda_traits<R (C::*)(Arg) const> {
    using argument_type = Arg;
    using result_type = R;
};

template <typename F>
struct lambda_traits : lambda_traits<decltype(&F::operator())> {};

template <typename R, typename Arg>
struct lambda_traits<R (*)(Arg)> {
    using argument_type = Arg;
    using result_type = R;
};

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
// signature_index — maps message type T to its index in Signatures...
// -----------------------------------------------------------------------------
template <typename T, typename... Signatures>
struct signature_index_impl;

template <typename T, typename R, typename... Rest>
struct signature_index_impl<T, result<R>(T), Rest...>
    : std::integral_constant<size_t, 0> {};

template <typename T, typename S, typename... Rest>
struct signature_index_impl<T, S, Rest...>
    : std::integral_constant<size_t,
                             1 + signature_index_impl<T, Rest...>::value> {};

template <typename T, typename... Signatures>
constexpr size_t signature_index_v =
    signature_index_impl<T, Signatures...>::value;

// matching_handler_type — maps a message type T to its result type in
// Signatures...
template <typename T, typename... Signatures>
struct matching_handler_type;

template <typename T, typename R, typename... Rest>
struct matching_handler_type<T, result<R>(T), Rest...> {
    using result = R;
};

template <typename T, typename S, typename... Rest>
struct matching_handler_type<T, S, Rest...>
    : matching_handler_type<T, Rest...> {};

// -----------------------------------------------------------------------------
// TypedBehavior - statically typed behavior for typed actors
// -----------------------------------------------------------------------------
template <typename... Signatures> class TypedBehavior {
  public:
    using result_type = void;

    TypedBehavior() = default;

    // Register a handler for message type Msg.
    // The handler signature F(Msg) -> R must match one of Signatures.
    template <typename F> TypedBehavior& on(F&& handler) {
        using decayed = std::decay_t<F>;
        // Extract Msg from the handler's argument type
        register_handler<decayed>(std::forward<F>(handler));
        return *this;
    }

    // Dispatch a message to the matching handler.
    template <typename Msg>
    result<typename matching_handler_type<std::decay_t<Msg>,
                                           Signatures...>::result>
    invoke_msg(Msg&& msg) {
        constexpr size_t idx =
            signature_index_v<std::decay_t<Msg>, Signatures...>;
        auto& handler = std::get<idx>(handlers_);
        return handler(std::forward<Msg>(msg));
    }

  private:
    template <typename F, typename Msg>
    void register_handler_impl(F&& handler) {
        constexpr size_t idx =
            signature_index_v<std::decay_t<Msg>, Signatures...>;
        std::get<idx>(handlers_).assign(std::forward<F>(handler));
    }

    template <typename F>
    void register_handler(F&& handler) {
        using arg_t =
            typename lambda_traits<std::decay_t<F>>::argument_type;
        register_handler_impl<F, arg_t>(std::forward<F>(handler));
    }

    std::tuple<message_handler<Signatures>...> handlers_;
};

// -----------------------------------------------------------------------------
// message_handler - handler for a single typed signature
// -----------------------------------------------------------------------------
template <typename R, typename Msg> class message_handler<result<R>(Msg)> {
  public:
    using signature = result<R>(Msg);
    using function_type = std::function<R(Msg)>;

    message_handler() = default;

    void assign(function_type fn) { fn_ = std::move(fn); }

    result<R> operator()(Msg&& msg) {
        if (fn_) {
            return result<R>::make(fn_(std::move(msg)));
        }
        return result<R>::make(error{});
    }

  private:
    function_type fn_;
};

// Specialization for void result — handler returns nothing
template <typename Msg> class message_handler<result<void>(Msg)> {
  public:
    using signature = result<void>(Msg);
    using function_type = std::function<void(Msg)>;

    message_handler() = default;

    void assign(function_type fn) { fn_ = std::move(fn); }

    result<void> operator()(Msg&& msg) {
        if (fn_) {
            fn_(std::move(msg));
            return result<void>::make();
        }
        return result<void>::make(error{});
    }

  private:
    function_type fn_;
};

} // namespace hpactor