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
#include <memory>
#include <unordered_map>

#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>

namespace hpactor {

/// \brief Helper for \c std::visit with lambdas (C++20 backport of
///        P0051R3 \c overloaded pattern).
///
/// Usage: \code{.cpp}
/// std::visit(overloaded{
///     [](int i) { ... },
///     [](std::string s) { ... },
/// }, my_variant);
/// \endcode
template <class... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

/// \brief Message handler collection with fluent builder API, \c become()
///        support, and composable behavior combinators.
///
/// \c Behavior is the primary unit of actor message handling. It supports
/// three styles of handler registration and composition:
///
/// **Fluent builder API (recommended for typed handlers):**
/// \code{.cpp}
/// Behavior make_behavior() override {
///     return Behavior::make()
///         .on_request<Lookup, LookupResponse>(
///             [this](const Lookup& req) { ... })
///         .on<Invalidate>([this](const Invalidate& inv) { ... });
/// }
/// \endcode
///
/// **Legacy raw-handler constructor (for backward compatibility):**
/// \code{.cpp}
/// return Behavior{[this](TypedMessage& msg) {
///     // manual type_id() dispatch
/// }};
/// \endcode
///
/// **Combinator API — composable decorators (new):**
/// \code{.cpp}
/// Behavior make_behavior() override {
///     return Behavior::setup([this]() {
///         auto child = context()->spawn(...);
///         return Behavior::intercept(
///             Behavior::make()
///                 .on<DoWork>([this](const DoWork& cmd) { ... }),
///             [](TypedMessage& msg, Behavior::next_fn next) {
///                 // pre-processing
///                 next(msg);
///                 // post-processing
///             });
///     });
/// }
/// \endcode
///
/// Dispatch order: typed handlers (registered via \c on<T>() and
/// \c on_request<ReqT,ResT>()) are checked first. If no typed handler
/// matches, the raw fallback handler (set via the constructor) is invoked.
/// When a combinator wrapper is active (intercept, compose, on_signal,
/// setup), dispatch routes through the wrapper first.
///
/// \note Thread safety: Assigning and invoking a \c Behavior must happen on
///       the actor's scheduler thread. No internal synchronization.
class Behavior {
  public:
    /// \brief Signature of the wrapped fallback handler function.
    using handler_type = std::function<void(TypedMessage&)>;

    /// \brief Construct an empty behavior that drops all messages.
    Behavior() = default;

    /// \brief Construct a behavior wrapping a raw fallback \p handler.
    ///
    /// The fallback is invoked only when no typed handler matches.
    /// \param[in] handler Callable invoked for each unmatched message.
    explicit Behavior(handler_type handler) : handler_(std::move(handler)) {}

    /// \brief Factory that returns an empty \c Behavior ready for fluent
    ///        chaining via \c on<T>() and \c on_request<ReqT,ResT>().
    static Behavior make() {
        return Behavior{};
    }

    /// \brief Register a fire-and-forget handler for a protobuf message type.
    ///
    /// The handler receives a const reference to the deserialized message.
    /// Use for one-way messages that do not expect a reply.
    ///
    /// \tparam T Protobuf message type (must have \c MessageTraits<T>
    ///           specialization).
    /// \param[in] handler Callable invoked with a const reference to the
    ///                    deserialized message.
    /// \return Reference to \c *this for fluent chaining.
    template <typename T> Behavior& on(std::function<void(const T&)> handler) {
        TypeTag tag = MessageTraits<T>::tag();
        typed_handlers_[tag] = [h = std::move(handler)](TypedMessage& msg) {
            auto parsed = msg.as<T>();
            if (parsed) {
                h(*parsed);
            }
        };
        return *this;
    }

    /// \brief Register a request-response handler for a protobuf message type.
    ///
    /// The handler receives a const reference to the deserialized request.
    /// Use \c context()->reply() inside the handler to send a response.
    ///
    /// \tparam ReqT Request protobuf message type (must have
    ///              \c MessageTraits<ReqT> specialization).
    /// \tparam ResT Expected response protobuf message type (for documentation
    ///              and future compile-time checking; not used at runtime).
    /// \param[in] handler Callable invoked with a const reference to the
    ///                    deserialized request.
    /// \return Reference to \c *this for fluent chaining.
    template <typename ReqT, typename ResT>
    Behavior& on_request(std::function<void(const ReqT&)> handler) {
        TypeTag tag = MessageTraits<ReqT>::tag();
        typed_handlers_[tag] = [h = std::move(handler)](TypedMessage& msg) {
            auto parsed = msg.as<ReqT>();
            if (parsed) {
                h(*parsed);
            }
        };
        return *this;
    }

    /// \brief Returns \c true if any handler (typed, fallback, or combinator)
    ///        is set.
    explicit operator bool() const {
        return !typed_handlers_.empty() || handler_ != nullptr ||
               compose_ != nullptr;
    }

    /// \brief Dispatch \p msg to the matching typed handler, or fall back to
    ///        the raw handler.
    ///
    /// When a combinator wrapper is active (intercept, compose, on_signal,
    /// setup), dispatch routes through the wrapper first. Otherwise, typed
    /// handlers are checked first by \c TypeTag. If no typed handler matches,
    /// the raw fallback handler (if set) is invoked. No-op if the behavior is
    /// completely empty.
    ///
    /// \param[in,out] msg The incoming typed message.
    void operator()(TypedMessage& msg) const {
        // Combinator path: delegate to the composition wrapper.
        if (compose_) {
            compose_->invoke(msg, *this);
            return;
        }
        // Standard path: typed handlers first, then fallback.
        auto it = typed_handlers_.find(msg.type_id());
        if (it != typed_handlers_.end()) {
            it->second(msg);
            return;
        }
        if (handler_) {
            handler_(msg);
        }
    }

    // ═════════════════════════════════════════════════════════════
    // Combinator factories
    // ═════════════════════════════════════════════════════════════

    /// \brief Named factory for a simple receive behavior.
    ///
    /// Equivalent to the raw-handler constructor. Provided for symmetry
    /// with the combinator API.
    /// \param[in] handler Callable invoked for each message.
    /// \return A \c Behavior that invokes \p handler for every message.
    static Behavior receive(handler_type handler) {
        return Behavior(std::move(handler));
    }

    /// \brief Explicit factory for an empty (no-op) behavior.
    ///
    /// Equivalent to the default constructor. All messages are dropped.
    /// \return An empty \c Behavior that evaluates to \c false.
    static Behavior empty() {
        return Behavior{};
    }

    /// \brief Signature for the passthrough callback used by
    ///        \c intercept().
    ///
    /// Call \c next(msg) to delegate to the inner behavior's full dispatch
    /// (typed handlers + fallback).
    using next_fn = std::function<void(TypedMessage&)>;

    /// \brief Intercept combinator — wrap a behavior with pre/post
    ///        processing (middleware pattern).
    ///
    /// The \p interceptor receives every message and a \c next_fn.
    /// Call \c next(msg) to delegate to the inner behavior's full
    /// dispatch (typed handlers + fallback). Suppress the call to
    /// filter out a message.
    ///
    /// \param[in] inner The behavior to wrap.
    /// \param[in] interceptor Callable invoked for each message.
    ///   Receives the message and a \c next_fn.
    /// \return A new \c Behavior that passes all messages through
    ///         \p interceptor first.
    static Behavior
    intercept(Behavior inner,
              std::function<void(TypedMessage&, next_fn)> interceptor);

    /// \brief Compose combinator — chain two behaviors so both receive
    ///        every message.
    ///
    /// \p first is invoked first, then \p second. Both see every message.
    /// Use for layering independent concerns (e.g., logging + handling).
    ///
    /// \param[in] first First behavior in the chain.
    /// \param[in] second Second behavior in the chain.
    /// \return A new \c Behavior that invokes \p first then \p second.
    static Behavior compose(Behavior first, Behavior second);

    /// \brief Signal combinator — intercept messages matching a
    ///        \c TypeTag and route them to a dedicated handler.
    ///
    /// Messages whose \c TypeTag equals \p tag are consumed by
    /// \p handler and NOT forwarded to \p inner. All other messages
    /// pass through to \p inner unchanged.
    ///
    /// \param[in] tag The \c TypeTag to match.
    /// \param[in] handler Callable invoked for matching signals.
    /// \param[in] inner The behavior to delegate non-matching
    ///                  messages to.
    /// \return A new \c Behavior with signal interception.
    static Behavior on_signal(TypeTag tag, handler_type handler, Behavior inner);

    /// \brief Setup combinator — defer behavior creation until the
    ///        first message arrives.
    ///
    /// The \p factory is called at most once (on first invocation).
    /// Until then the behavior is non-empty but defers all messages.
    /// Useful for one-time initialization that requires actor context
    /// (spawning children, etc.).
    ///
    /// \param[in] factory Callable that returns the real \c Behavior.
    ///   Called on first message; the returned behavior is cached and
    ///   used for all subsequent messages.
    /// \return A \c Behavior that lazily initializes from \p factory.
    static Behavior setup(std::function<Behavior()> factory);

    /// \brief Create a message adapter combinator.
    ///
    /// Messages of type \c From are translated to type \c To via
    /// \p adapter_fn before being dispatched to \p inner. All other
    /// message types pass through unchanged.
    template <typename From, typename To>
    static Behavior
    message_adapter(std::function<To(const From&)> adapter_fn, Behavior inner) {
        Behavior result;
        auto state = std::make_shared<ComposeState>();
        state->type = ComposeState::Type::MessageAdapter;
        state->inner = std::make_shared<Behavior>(std::move(inner));
        state->adapter_from_tag = MessageTraits<From>::tag();
        state->adapter_fn = [fn = std::move(adapter_fn)](
                                const TypedMessage& msg) -> TypedMessage {
            auto proto = msg.as<From>();
            To to = fn(*proto);
            return TypedMessage(MessageTraits<To>::tag(), to);
        };
        result.compose_ = std::move(state);
        return result;
    }

  private:
    using typed_handler_type = std::function<void(TypedMessage&)>;
    std::unordered_map<TypeTag, typed_handler_type> typed_handlers_;
    handler_type handler_;

    // ── Composition support ────────────────────────────────────

    /// \brief Internal type-erased composition node.
    ///
    /// When non-null, \c operator() delegates to
    /// \c compose_->invoke(msg, *this) instead of the normal
    /// typed-handler → fallback path.  This allows combinators to
    /// intercept every message before any dispatch.
    struct ComposeState {
        enum class Type : uint8_t {
            Intercept,
            Compose,
            OnSignal,
            Setup,
            MessageAdapter
        };

        Type type;
        /// Inner behavior — the wrapped/decorated behavior.
        /// Stored as shared_ptr because Behavior is incomplete
        /// at this point in the class definition.
        std::shared_ptr<Behavior> inner;
        /// For \c Compose: second behavior in the chain.
        std::shared_ptr<Behavior> second;
        /// For \c Intercept: the interceptor callback.
        std::function<void(TypedMessage&, next_fn)> interceptor;
        /// For \c OnSignal: tag to match.
        TypeTag signal_tag = TypeTag::Invalid;
        /// For \c OnSignal: handler for matching signals.
        handler_type signal_handler;
        /// For \c Setup: has the factory been called?
        bool initialized = false;
        /// For \c Setup: the factory function.
        std::function<Behavior()> factory;
        /// For \c MessageAdapter: translate From → To messages.
        std::function<TypedMessage(const TypedMessage&)> adapter_fn;
        /// For \c MessageAdapter: the source TypeTag to match.
        TypeTag adapter_from_tag = TypeTag::Invalid;

        /// \brief Entry point for combinator dispatch.
        ///
        /// Dispatches according to \c type:
        /// - \c Intercept: calls \c interceptor(msg, next) where
        ///   \c next delegates to \c inner.
        /// - \c MessageAdapter: translates matching messages, passes
        ///   through others.
        /// - \c Compose: calls \c inner then \c second.
        /// - \c OnSignal: routes matching tags to \c signal_handler,
        ///   others to \c inner.
        /// - \c Setup: lazily initialises \c inner from \c factory,
        ///   then delegates to it.
        void invoke(TypedMessage& msg, const Behavior& self);
    };
    std::shared_ptr<ComposeState> compose_;
};

} // namespace hpactor
