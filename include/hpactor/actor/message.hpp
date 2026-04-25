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

#include <hpactor/types/types.hpp>

#include <atomic>
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

    // Copy and move constructors - needed because the templated constructor
    // below would otherwise hide the implicit members.
    Message(const Message& other) : payload_(other.payload_) {}
    Message(Message&& other) noexcept : payload_(std::move(other.payload_)) {}

    // Copy and move assignment - needed when a user-declared move constructor
    // deletes the implicit copy assignment operator
    Message& operator=(const Message& other) {
        payload_ = other.payload_;
        return *this;
    }
    Message& operator=(Message&& other) noexcept {
        payload_ = std::move(other.payload_);
        return *this;
    }

    // Perfect forwarding constructor - handles payload types that are not
    // Message
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

    // Intrusive link for MPSCMailbox (name must be mpsc_next, no underscore)
    std::atomic<Message*> mpsc_next{nullptr};

  private:
    T payload_;
};

} // namespace hpactor