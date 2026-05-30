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

#include <hpactor/fault/fault_macros.hpp>

#include <cstdint>
#include <deque>
#include <mutex>

namespace hpactor::mailbox {

/// \brief Point-in-time snapshot of an OverflowQueue for observability.
///
/// All counters are captured atomically under the queue's internal mutex.
struct OverflowQueueSnapshot {
    uint32_t depth = 0;           ///< Current number of elements in the queue.
    uint32_t max_depth = 0;       ///< Configured maximum depth (0 = unlimited).
    uint64_t total_pushed = 0;    ///< Cumulative push count since construction.
    uint64_t total_popped = 0;    ///< Cumulative successful pop count since construction.
    uint64_t total_lost = 0;      ///< Cumulative elements silently evicted on overflow.
};

/// \brief Bounded FIFO overflow queue for mailbox spill-over.
///
/// When the main lock-free mailbox is at capacity, SpillToOverflowQueue policy
/// routes messages through this secondary queue.  On dequeue, overflow entries
/// drain back into the main mailbox in FIFO order.
///
/// Evicts the oldest entry when \p max_depth is exceeded (0 = unlimited).
/// All operations are internally synchronized with a \c std::mutex.
///
/// \tparam T Message type; must be move-constructible and move-assignable.
///
/// \note Thread safety: externally synchronized via internal mutex.
///       Safe to call from any thread.
template <typename T>
class OverflowQueue {
  public:
    /// \brief Construct an overflow queue with an optional depth limit.
    ///
    /// \param[in] max_depth Maximum number of elements (0 = unlimited).
    explicit OverflowQueue(uint32_t max_depth = 0) noexcept
        : max_depth_(max_depth) {}

    /// \brief Update the maximum depth at runtime.
    ///
    /// Does not trim existing elements; the new limit takes effect on the
    /// next \c try_push().
    ///
    /// \param[in] max_depth New maximum depth (0 = unlimited).
    void set_max_depth(uint32_t max_depth) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        max_depth_ = max_depth;
    }

    /// \brief Return the configured maximum depth.
    ///
    /// \return Current max depth (0 = unlimited).
    /// \note Thread safety: lock-free read of the max_depth snapshot;
    ///       the actual limit may change concurrently if \c set_max_depth()
    ///       is called from another thread.
    [[nodiscard]] uint32_t max_depth() const noexcept {
        return max_depth_;
    }

    /// \brief Push a message into the overflow queue.
    ///
    /// Always succeeds: if the queue is at \c max_depth, the oldest entry
    /// is silently evicted to make room.
    ///
    /// \param[in] msg Message to push (moved into the queue).
    /// \retval true Always returns true.
    /// \note Thread safety: acquires internal mutex.
    bool try_push(T&& msg) noexcept {
        FAULT_INJECT("hpactor.mailbox.overflow.push.drop") {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (max_depth_ > 0 && queue_.size() >= max_depth_) {
            if (!queue_.empty()) {
                queue_.pop_front();
                total_lost_++;
            }
        }
        queue_.push_back(std::move(msg));
        total_pushed_++;
        return true;
    }

    /// \brief Pop the oldest message from the overflow queue.
    ///
    /// \param[out] out Set to the popped message on success; untouched on
    ///                  failure.
    /// \retval true A message was popped; \p out holds the moved value.
    /// \retval false The queue is empty; \p out is unchanged.
    /// \note Thread safety: acquires internal mutex.
    bool try_pop(T& out) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        total_popped_++;
        return true;
    }

    /// \brief Check whether the overflow queue is empty.
    ///
    /// \retval true No elements are queued.
    /// \retval false At least one element is queued.
    /// \note Thread safety: acquires internal mutex.
    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /// \brief Return the current number of elements.
    ///
    /// \return Current queue depth.
    /// \note Thread safety: acquires internal mutex.
    [[nodiscard]] uint32_t depth() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<uint32_t>(queue_.size());
    }

    /// \brief Capture a point-in-time snapshot of all counters and depth.
    ///
    /// All fields are read atomically under the internal mutex.
    ///
    /// \return Snapshot with current depth, max_depth, and cumulative counters.
    /// \note Thread safety: acquires internal mutex.
    [[nodiscard]] OverflowQueueSnapshot snapshot() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return {static_cast<uint32_t>(queue_.size()), max_depth_,
                total_pushed_, total_popped_, total_lost_};
    }

  private:
    mutable std::mutex mutex_;
    std::deque<T> queue_;
    uint32_t max_depth_{0};
    uint64_t total_pushed_{0};
    uint64_t total_popped_{0};
    uint64_t total_lost_{0};
};

} // namespace hpactor::mailbox
