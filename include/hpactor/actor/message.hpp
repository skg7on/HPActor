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
#include <type_traits>
#include <utility>

namespace hpactor {

// Error codes for hot-path operations (no exceptions)
enum class MailboxError {
    Success,
    Full,   // Mailbox at capacity
    Empty,  // Mailbox empty on pop
    Invalid // Invalid state
};

template <typename T> class Message {
  public:
    Message() = default;

    // Perfect forwarding constructor - handles both copy and move
    template <typename U>
    explicit Message(U&& payload) : payload_(std::forward<U>(payload)) {}

    T& payload() noexcept {
        return payload_;
    }
    const T& payload() const noexcept {
        return payload_;
    }

    // Allow explicit access to underlying payload move
    T&& move_payload() noexcept {
        return std::move(payload_);
    }

  private:
    T payload_;
};

} // namespace hpactor