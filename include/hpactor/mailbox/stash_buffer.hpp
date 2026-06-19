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

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

#include <hpactor/msg/typed_message.hpp>

namespace hpactor {

/// \brief Bounded FIFO buffer for stashing messages during actor
///        initialization or state transitions.
///
/// Provides a temporary holding area for \c TypedMessage objects
/// that arrive while the actor is not ready to process them.
/// Messages can be stashed with \c try_stash() and later replayed
/// via \c unstash_all() or \c unstash_one().
///
/// Common patterns:
/// - **Initialization gating**: stash all messages until setup
///   completes, then unstash.
/// - **State machine transitions**: stash messages that arrive
///   during a transition, then unstash after the new state is
///   entered.
///
/// \note Thread safety: Not internally synchronized. Must be used
///       from the actor's scheduler thread.
class StashBuffer {
  public:
    /// \brief Construct a stash buffer with bounded capacity.
    ///
    /// \param[in] capacity Maximum number of messages to hold.
    ///                     Zero-capacity buffers are valid but
    ///                     always full.
    explicit StashBuffer(size_t capacity) : capacity_(capacity) {}

    /// \brief Move constructor.
    StashBuffer(StashBuffer&&) noexcept = default;

    /// \brief Move assignment.
    StashBuffer& operator=(StashBuffer&&) noexcept = default;

    // Non-copyable (TypedMessage is move-only).
    StashBuffer(const StashBuffer&) = delete;
    StashBuffer& operator=(const StashBuffer&) = delete;

    /// \brief Try to stash a message.
    ///
    /// \param[in] msg The message to stash (moved into the buffer).
    /// \return \c true if the message was stashed.
    /// \retval false The buffer is full; the message was dropped.
    bool try_stash(TypedMessage msg) {
        if (buf_.size() >= capacity_)
            return false;
        buf_.push_back(std::move(msg));
        return true;
    }

    /// \brief Remove and return the oldest stashed message (FIFO).
    ///
    /// \return The oldest message, or \c std::nullopt if empty.
    std::optional<TypedMessage> unstash_one() {
        if (buf_.empty())
            return std::nullopt;
        auto msg = std::move(buf_.front());
        buf_.pop_front();
        return msg;
    }

    /// \brief Remove and return all stashed messages in FIFO order.
    ///
    /// After this call the buffer is empty.
    /// \return A vector of all stashed messages (oldest first).
    std::vector<TypedMessage> unstash_all() {
        std::vector<TypedMessage> result;
        result.reserve(buf_.size());
        while (!buf_.empty()) {
            result.push_back(std::move(buf_.front()));
            buf_.pop_front();
        }
        return result;
    }

    /// \brief Drop all stashed messages without processing them.
    void clear() {
        buf_.clear();
    }

    /// \brief Number of messages currently stashed.
    [[nodiscard]] size_t size() const noexcept {
        return buf_.size();
    }

    /// \brief \c true when no messages are stashed.
    [[nodiscard]] bool empty() const noexcept {
        return buf_.empty();
    }

    /// \brief \c true when the buffer is at capacity.
    [[nodiscard]] bool full() const noexcept {
        return buf_.size() >= capacity_;
    }

    /// \brief Maximum number of messages this buffer can hold.
    [[nodiscard]] size_t capacity() const noexcept {
        return capacity_;
    }

  private:
    std::deque<TypedMessage> buf_;
    size_t capacity_;
};

} // namespace hpactor
