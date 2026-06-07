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

/// \brief Primary template for extracting argument and result types from
///        a callable's \c operator().
///
/// \tparam F The callable type (lambda or function object).
template <typename F> struct lambda_traits;

/// \brief Specialization for non-const member function pointers.
///
/// \tparam C The class type.
/// \tparam R The return type of the call operator.
/// \tparam Arg The single argument type of the call operator.
template <typename C, typename R, typename Arg>
struct lambda_traits<R (C::*)(Arg)> {
    using argument_type = Arg;
    using result_type = R;
};

/// \brief Specialization for const member function pointers.
///
/// \tparam C The class type.
/// \tparam R The return type of the call operator.
/// \tparam Arg The single argument type of the call operator.
template <typename C, typename R, typename Arg>
struct lambda_traits<R (C::*)(Arg) const> {
    using argument_type = Arg;
    using result_type = R;
};

/// \brief Derives from the appropriate specialization by inspecting
///        \c &F::operator().
///
/// \tparam F The callable type.
template <typename F>
struct lambda_traits : lambda_traits<decltype(&F::operator())> {};

/// \brief Specialization for free function pointers.
///
/// \tparam R The return type.
/// \tparam Arg The single argument type.
template <typename R, typename Arg> struct lambda_traits<R (*)(Arg)> {
    using argument_type = Arg;
    using result_type = R;
};

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------

/// \brief Handler for a single typed signature \c result<R>(Msg).
///
/// \tparam Signature A function type of the form \c result<R>(Msg).
template <typename Signature> class message_handler;

// -----------------------------------------------------------------------------
// handler_type - extracts result type from a typed message signature
// -----------------------------------------------------------------------------

/// \brief Primary template: extracts the result and message types from
///        a signature of the form \c result<R>(Msg).
///
/// \tparam T The signature type.
template <typename T> struct handler_type;

/// \brief Specialization for \c result<R>(Msg).
///
/// \tparam R The result type.
/// \tparam Msg The message type.
template <typename R, typename Msg> struct handler_type<result<R>(Msg)> {
    /// \brief The result type \c R.
    using result = R;
    /// \brief The message type \c Msg.
    using message = Msg;

    /// \brief Invoke a handler function with a message.
    ///
    /// \tparam F The handler callable type.
    /// \param[in] f The handler function.
    /// \param[in] msg The message to pass (moved).
    /// \return The result of \c f(std::move(msg)).
    template <typename F> result operator()(F&& f, Msg&& msg) {
        return f(std::move(msg));
    }
};

// -----------------------------------------------------------------------------
// signature_index — maps message type T to its index in Signatures...
// -----------------------------------------------------------------------------

/// \brief Primary template: recursively computes the index of message
///        type \c T in the signature pack \c Signatures.
///
/// \tparam T The message type to locate.
/// \tparam Signatures The signature pack to search.
template <typename T, typename... Signatures> struct signature_index_impl;

/// \brief Base case: \c T matches the first signature's message type.
///        Index is 0.
///
/// \tparam T The matched message type.
/// \tparam R The result type of the matched signature.
/// \tparam Rest Remaining (unused) signatures.
template <typename T, typename R, typename... Rest>
struct signature_index_impl<T, result<R>(T), Rest...>
    : std::integral_constant<size_t, 0> {};

/// \brief Recursive case: \c T does not match the current head.
///        Index is 1 + the index in the tail.
///
/// \tparam T The message type to locate.
/// \tparam S The current (non-matching) signature head.
/// \tparam Rest Remaining signatures to search.
template <typename T, typename S, typename... Rest>
struct signature_index_impl<T, S, Rest...>
    : std::integral_constant<size_t, 1 + signature_index_impl<T, Rest...>::value> {
};

/// \brief Compile-time index of message type \c T in \c Signatures.
///
/// \tparam T The message type to locate.
/// \tparam Signatures The signature pack.
template <typename T, typename... Signatures>
constexpr size_t signature_index_v = signature_index_impl<T, Signatures...>::value;

// -----------------------------------------------------------------------------
// matching_handler_type — maps a message type T to its result type
// -----------------------------------------------------------------------------

/// \brief Primary template: maps message type \c T to its result type
///        \c R by matching against the signature pack.
///
/// \tparam T The message type to match.
/// \tparam Signatures The signature pack.
// matching_handler_type — maps a message type T to its result type in
// Signatures...
template <typename T, typename... Signatures> struct matching_handler_type;

/// \brief Base case: \c T matches the first signature. Exposes the
///        result type \c R.
///
/// \tparam T The matched message type.
/// \tparam R The result type of the matched signature.
/// \tparam Rest Remaining (unused) signatures.
template <typename T, typename R, typename... Rest>
struct matching_handler_type<T, result<R>(T), Rest...> {
    /// \brief The result type \c R from the matched signature.
    using result = R;
};

/// \brief Recursive case: skip the current head and search the tail.
///
/// \tparam T The message type to match.
/// \tparam S The current (non-matching) signature head.
/// \tparam Rest Remaining signatures.
template <typename T, typename S, typename... Rest>
struct matching_handler_type<T, S, Rest...> : matching_handler_type<T, Rest...> {
};

// -----------------------------------------------------------------------------
// TypedBehavior - statically typed behavior for typed actors
// -----------------------------------------------------------------------------

/// \brief Statically typed behavior that dispatches messages by
///        compile-time type to registered handlers.
///
/// Stores a \c std::tuple of \c message_handler objects, one per
/// signature. Handlers are registered by type via \c on() and
/// dispatched via \c invoke_msg() using compile-time index lookup
/// (\c signature_index_v).
///
/// \tparam Signatures A pack of \c result<R>(Msg) function types
///         defining the actor's message contract.
/// \note Actor-confined: designed to be called only from the owning
///       actor's message-processing path. No internal synchronization.
// Statically typed behavior for typed actors
template <typename... Signatures> class TypedBehavior {
  public:
    /// \brief Result type alias (always \c void for behaviors).
    using result_type = void;

    /// \brief Default constructible. No handlers are registered until
    ///        \c on() is called.
    TypedBehavior() = default;

    /// \brief Register a handler for a message type inferred from the
    ///        handler's argument type.
    ///
    /// Uses \c lambda_traits to extract the message type from \p handler.
    /// The handler signature \c F(Msg) -> R must match one of
    /// \c Signatures. Returns \c *this for chaining.
    ///
    /// \tparam F The handler callable type (lambda or function object).
    /// \param[in] handler The handler function.
    /// \return Reference to \c *this for fluent registration.
    // Register a handler for message type Msg.
    // The handler signature F(Msg) -> R must match one of Signatures.
    template <typename F> TypedBehavior& on(F&& handler) {
        using decayed = std::decay_t<F>;
        // Extract Msg from the handler's argument type
        register_handler<decayed>(std::forward<F>(handler));
        return *this;
    }

    /// \brief Dispatch a message to the matching handler by compile-time
    ///        type.
    ///
    /// Looks up the handler index at compile time via
    /// \c signature_index_v and invokes the handler at that tuple index.
    /// If no handler was registered for this message type, the
    /// \c message_handler returns \c result<R>::make(error{}).
    ///
    /// \tparam Msg The message type to dispatch (decayed).
    /// \param[in] msg The message to forward to the handler.
    /// \return The result of the matching handler, or an error result if
    ///         no handler is registered.
    // Dispatch a message to the matching handler.
    template <typename Msg>
    result<typename matching_handler_type<std::decay_t<Msg>, Signatures...>::result>
    invoke_msg(Msg&& msg) {
        constexpr size_t idx = signature_index_v<std::decay_t<Msg>, Signatures...>;
        auto& handler = std::get<idx>(handlers_);
        return handler(std::forward<Msg>(msg));
    }

  private:
    /// \brief Register a handler for a specific message type.
    ///
    /// \tparam F The handler callable type.
    /// \tparam Msg The message type to register for.
    /// \param[in] handler The handler function (forwarded).
    template <typename F, typename Msg>
    void register_handler_impl(F&& handler) {
        constexpr size_t idx = signature_index_v<std::decay_t<Msg>, Signatures...>;
        std::get<idx>(handlers_).assign(std::forward<F>(handler));
    }

    /// \brief Extract the message type from the handler and delegate to
    ///        \c register_handler_impl.
    ///
    /// \tparam F The handler callable type.
    /// \param[in] handler The handler function.
    template <typename F> void register_handler(F&& handler) {
        using arg_t = typename lambda_traits<std::decay_t<F>>::argument_type;
        register_handler_impl<F, arg_t>(std::forward<F>(handler));
    }

    /// \brief Tuple of message handlers, one per signature.
    ///
    /// Indexed by the compile-time position of the message type in
    /// \c Signatures.
    std::tuple<message_handler<Signatures>...> handlers_;
};

// -----------------------------------------------------------------------------
// message_handler - handler for a single typed signature
// -----------------------------------------------------------------------------

/// \brief Handler for a single typed signature \c result<R>(Msg).
///
/// Wraps a \c std::function<R(Msg)>. If no function is assigned,
/// \c operator() returns \c result<R>::make(error{}).
///
/// \tparam R The result type.
/// \tparam Msg The message type.
// message_handler - handler for a single typed signature
template <typename R, typename Msg> class message_handler<result<R>(Msg)> {
  public:
    /// \brief The signature type \c result<R>(Msg).
    using signature = result<R>(Msg);
    /// \brief The stored function type \c std::function<R(Msg)>.
    using function_type = std::function<R(Msg)>;

    /// \brief Default constructible. No handler assigned; calls return
    ///        an error result.
    message_handler() = default;

    /// \brief Assign a function to this handler slot.
    ///
    /// \param[in] fn The handler function (moved into the slot).
    void assign(function_type fn) {
        fn_ = std::move(fn);
    }

    /// \brief Invoke the handler with a message.
    ///
    /// \param[in] msg The message to pass (moved).
    /// \return \c result<R> containing the handler's return value, or
    ///         \c result<R>::make(error{}) if no handler is assigned.
    result<R> operator()(Msg&& msg) {
        if (fn_) {
            return result<R>::make(fn_(std::move(msg)));
        }
        return result<R>::make(error{});
    }

  private:
    /// \brief The stored handler function, or empty if unassigned.
    function_type fn_;
};

/// \brief Specialization for \c void result — the handler returns
///        nothing.
///
/// Wraps a \c std::function<void(Msg)>. On success, returns a default
/// \c result<void>.
///
/// \tparam Msg The message type.
// Specialization for void result — handler returns nothing
template <typename Msg> class message_handler<result<void>(Msg)> {
  public:
    /// \brief The signature type \c result<void>(Msg).
    using signature = result<void>(Msg);
    /// \brief The stored function type \c std::function<void(Msg)>.
    using function_type = std::function<void(Msg)>;

    /// \brief Default constructible.
    message_handler() = default;

    /// \brief Assign a function to this handler slot.
    ///
    /// \param[in] fn The handler function (moved into the slot).
    void assign(function_type fn) {
        fn_ = std::move(fn);
    }

    /// \brief Invoke the handler with a message.
    ///
    /// \param[in] msg The message to pass (moved).
    /// \return \c result<void>::make() on success, or
    ///         \c result<void>::make(error{}) if no handler is assigned.
    result<void> operator()(Msg&& msg) {
        if (fn_) {
            fn_(std::move(msg));
            return result<void>::make();
        }
        return result<void>::make(error{});
    }

  private:
    /// \brief The stored handler function, or empty if unassigned.
    function_type fn_;
};

} // namespace hpactor
