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

// -----------------------------------------------------------------------------
// overloaded - helper for std::visit with lambdas (C++20 backport)
// -----------------------------------------------------------------------------
template <class... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// -----------------------------------------------------------------------------
// Behavior - handler function for message processing
// -----------------------------------------------------------------------------
class Behavior {
  public:
    using handler_type = std::function<void(TypedMessage&)>;

    Behavior() = default;

    explicit Behavior(handler_type handler) : handler_(std::move(handler)) {}

    explicit operator bool() const {
        return handler_ != nullptr;
    }

    void operator()(TypedMessage& msg) const {
        if (handler_) {
            handler_(msg);
        }
    }

  private:
    handler_type handler_;
};

} // namespace hpactor
