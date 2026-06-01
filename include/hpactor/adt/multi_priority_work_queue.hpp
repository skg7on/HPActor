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

#include <cstdint>
#include <vector>

#include <hpactor/adt/chaselev_deque.hpp>

namespace hpactor::adt {

/// \brief Multi-priority work queue backed by an array of ChaseLev deques.
///
/// Priority 0 is the highest. The owning thread pops from the highest-priority
/// non-empty deque (LIFO), while thief threads steal from the highest-priority
/// non-empty deque (FIFO).
///
/// \tparam T Element type. Must meet the requirements of ChaselevDeque<T>.
///
/// \note Concurrency: delegates to ChaselevDeque. push() and pop() are
///       single-owner; steal() is safe for concurrent thief threads.
template <typename T> class MultiPriorityWorkQueue {
  public:
    /// \brief Construct a queue with the given number of priority levels.
    ///
    /// \param[in] priority_levels Number of priority levels (default 4).
    ///            Level 0 is the highest priority.
    explicit MultiPriorityWorkQueue(uint32_t priority_levels = 4)
        : levels_(priority_levels) {}

    /// \brief Push an item onto the bottom of the given priority level.
    ///
    /// \param[in] priority Priority level (0 = highest).
    /// \param[in] item Element to push.
    void push(uint8_t priority, T item) {
        levels_[priority].push_bottom(std::move(item));
    }

    /// \brief Pop an item from the highest-priority non-empty level.
    ///
    /// Searches levels from 0 upward, popping the first non-empty deque.
    /// \param[out] out Set to the popped element on success.
    /// \retval true An element was popped into \p out.
    /// \retval false All levels are empty.
    bool pop(T& out) {
        for (uint32_t i = 0; i < levels_.size(); ++i) {
            if (levels_[i].pop_bottom(out)) {
                return true;
            }
        }
        return false;
    }

    /// \brief Steal an item from the highest-priority non-empty level.
    ///
    /// Searches levels from 0 upward, stealing the first non-empty deque.
    /// \param[out] out Set to the stolen element on success.
    /// \retval true An element was stolen into \p out.
    /// \retval false All levels are empty.
    bool steal(T& out) {
        for (uint32_t i = 0; i < levels_.size(); ++i) {
            if (levels_[i].steal_top(out)) {
                return true;
            }
        }
        return false;
    }

    /// \brief Approximate total number of elements across all levels.
    ///
    /// This is a relaxed snapshot and may be stale by the time it is observed.
    /// \return Approximate total element count.
    size_t depth_approx() const {
        size_t total = 0;
        for (const auto& level : levels_) {
            total += level.size_approx();
        }
        return total;
    }

    /// \brief Number of priority levels in this queue.
    ///
    /// \return The priority level count.
    uint32_t num_levels() const {
        return static_cast<uint32_t>(levels_.size());
    }

  private:
    std::vector<ChaselevDeque<T>> levels_;
};

} // namespace hpactor::adt
