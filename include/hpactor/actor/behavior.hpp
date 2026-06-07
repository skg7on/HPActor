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

namespace hpactor {

class TypedMessage;

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

/// \brief Message handler function wrapper with \c become() support.
///
/// The primary unit of actor behavior. Each \c Behavior wraps a callable
/// that receives a \c TypedMessage. Actors change behavior by assigning a
/// new \c Behavior (the "become" pattern). A default-constructed \c Behavior
/// is empty and evaluates to \c false.
///
/// \note Thread safety: Assigning and invoking a \c Behavior must happen on
///       the actor's scheduler thread. No internal synchronization.
class Behavior {
  public:
    /// \brief Signature of the wrapped handler function.
    using handler_type = std::function<void(TypedMessage&)>;

    /// \brief Construct an empty behavior that drops all messages.
    Behavior() = default;

    /// \brief Construct a behavior wrapping \p handler.
    /// \param[in] handler Callable invoked for each message.
    explicit Behavior(handler_type handler) : handler_(std::move(handler)) {}

    /// \brief Returns \c true if a handler is set.
    explicit operator bool() const {
        return handler_ != nullptr;
    }

    /// \brief Invoke the wrapped handler with \p msg.
    ///
    /// No-op if the behavior is empty.
    /// \param[in,out] msg The incoming typed message.
    void operator()(TypedMessage& msg) const {
        if (handler_) {
            handler_(msg);
        }
    }

  private:
    handler_type handler_;
};

} // namespace hpactor
