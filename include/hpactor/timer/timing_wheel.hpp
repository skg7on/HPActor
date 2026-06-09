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

#include <hpactor/mem/std_allocator.hpp>
#include <hpactor/sched/work_queue.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace hpactor::sched {

// -----------------------------------------------------------------------------
// TimingWheel: Hierarchical timer wheel for efficient timeout management
// -----------------------------------------------------------------------------
// Implements a hierarchical timing wheel (similar to Linux kernel timer wheel)
// for O(1) timer insert and cancel operations.
//
// The wheel has multiple levels, each with a different granularity:
// - Level 0: 1ms resolution, 256 slots
// - Level 1: 256ms resolution, 256 slots
// - Level 2: ~65ms resolution, 256 slots (covers ~16 seconds)
// - Level 3: ~16s resolution, 256 slots (covers ~70 minutes)
//
// Each level covers a different range of time, allowing efficient management
// of timers from 1ms to hours.
// -----------------------------------------------------------------------------
class TimingWheel {
  public:
    using TimerCallback = std::function<void()>;

    struct Timer : mem::SlabAllocated<Timer> {
        int64_t expire_ns;      // absolute expiration time
        uint64_t id;            // unique timer id
        TimerCallback callback; // called when timer fires
        bool cancelled{false};  // set to true to cancel
    };

    // Configure wheel with tick duration and levels
    // tick_ns: duration of one tick at level 0 (e.g., 1ms = 1,000,000)
    // num_levels: number of wheel levels (default 4)
    explicit TimingWheel(int64_t tick_ns = 1'000'000, uint32_t num_levels = 4);

    // Destructor - cancels all pending timers
    ~TimingWheel();

    TimingWheel(const TimingWheel&) = delete;
    TimingWheel& operator=(const TimingWheel&) = delete;
    TimingWheel(TimingWheel&&) = delete;
    TimingWheel& operator=(TimingWheel&&) = delete;

    // Schedule a timer to fire after the given duration (in ns)
    // Returns a TimerHandle that can be used to cancel the timer
    uint64_t schedule(int64_t delay_ns, TimerCallback callback);

    // Schedule a timer to fire at the given absolute time
    uint64_t schedule_at(int64_t expire_ns, TimerCallback callback);

    // Cancel a previously scheduled timer
    // Returns true if timer was found and cancelled
    bool cancel(uint64_t timer_id);

    // Advance the wheel by the given number of ticks
    // Calls callbacks for all expired timers
    // Returns number of timers that fired
    uint32_t advance(int64_t now_ns);

    // Get current time according to the wheel
    int64_t current_time() const {
        return current_time_.load(std::memory_order_relaxed);
    }

    // Check if there are pending timers
    bool empty() const;

    // Number of pending timers (approximate)
    size_t size() const;

  private:
    struct WheelLevel {
        std::vector<std::vector<Timer*>> buckets;
        uint32_t num_buckets;
        uint32_t mask;
    };

    uint64_t add_timer_internal(int64_t expire_ns, TimerCallback callback);
    void cascade_level(uint32_t level, int64_t now);
    void insert_timer(Timer* timer);
    Timer* remove_timer(uint64_t timer_id);

    int64_t tick_ns_;
    uint32_t num_levels_;
    std::vector<WheelLevel> levels_;

    std::atomic<int64_t> current_time_{0};
    std::atomic<uint64_t> next_timer_id_{1};

    // All pending timers (for iteration and size)
    std::vector<Timer*> all_timers_;
    // For thread-safe timer management
    std::atomic<bool> timers_modified_{false};
    // Protects all bucket operations (schedule/cancel/advance may be called
    // from different threads).
    mutable std::recursive_mutex mutex_;
};

} // namespace hpactor::sched