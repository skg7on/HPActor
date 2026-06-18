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

/// \brief Message handler collection with fluent builder API and \c become()
///        support.
///
/// \c Behavior is the primary unit of actor message handling. It supports two
/// styles of handler registration:
///
/// **Fluent builder API (recommended):**
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
/// Dispatch order: typed handlers (registered via \c on<T>() and
/// \c on_request<ReqT,ResT>()) are checked first. If no typed handler
/// matches, the raw fallback handler (set via the constructor) is invoked.
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

    /// \brief Returns \c true if any handler (typed or fallback) is set.
    explicit operator bool() const {
        return !typed_handlers_.empty() || handler_ != nullptr;
    }

    /// \brief Dispatch \p msg to the matching typed handler, or fall back to
    ///        the raw handler.
    ///
    /// Typed handlers are checked first by \c TypeTag. If no typed handler
    /// matches, the raw fallback handler (if set) is invoked. No-op if the
    /// behavior is completely empty.
    ///
    /// \param[in,out] msg The incoming typed message.
    void operator()(TypedMessage& msg) const {
        auto it = typed_handlers_.find(msg.type_id());
        if (it != typed_handlers_.end()) {
            it->second(msg);
            return;
        }
        if (handler_) {
            handler_(msg);
        }
    }

  private:
    using typed_handler_type = std::function<void(TypedMessage&)>;
    std::unordered_map<TypeTag, typed_handler_type> typed_handlers_;
    handler_type handler_;
};

} // namespace hpactor
