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

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hpactor::adt {

/// \brief Configuration for the hierarchical timer-wheel CalendarQueue.
///
/// All bucket counts must be powers of two (constructor aborts otherwise).
struct CalendarQueueConfig {
    /// \brief Nanosecond granularity of the finest wheel level.
    int64_t fine_bucket_ns = 1'000'000; // 1ms
    /// \brief Number of buckets in the fine wheel (must be power of two).
    uint32_t fine_buckets = 256;
    /// \brief Number of buckets in the coarse wheel (must be power of two).
    uint32_t coarse_buckets = 256;
    /// \brief Number of buckets in the remote wheel (must be power of two).
    uint32_t remote_buckets = 256;
    /// \brief Maximum number of fine buckets to process per advance() call,
    ///        bounding the latency of a single tick.
    uint32_t max_advance_buckets = 4096;
};

/// \brief Hierarchical timer wheel with O(1) insert and cancel.
///
/// Three-level cascade: fine → coarse → remote. Timers that fall within
/// the fine wheel's range are inserted directly; longer-duration timers
/// land in coarse or remote buckets and cascade down as time advances.
///
/// \note Thread safety: externally synchronized via a recursive mutex.
///       Timer callbacks fire under the lock during advance(), so callbacks
///       may safely call schedule(), schedule_at(), or cancel().
class CalendarQueue {
  public:
    /// \brief Callback type invoked when a timer fires.
    using TimerCallback = std::function<void()>;

    /// \brief Construct a calendar queue with the given configuration.
    ///
    /// \pre All bucket counts in \p cfg must be powers of two.
    /// \param[in] cfg Wheel configuration (bucket counts, granularity, cap).
    explicit CalendarQueue(const CalendarQueueConfig& cfg = {});

    /// \brief Destroys the queue and all pending timers.
    ///
    /// Owned Timer objects are deleted. Callbacks for pending timers
    /// are not invoked.
    ~CalendarQueue();

    CalendarQueue(const CalendarQueue&) = delete;
    CalendarQueue& operator=(const CalendarQueue&) = delete;
    CalendarQueue(CalendarQueue&&) = delete;
    CalendarQueue& operator=(CalendarQueue&&) = delete;

    /// \brief Schedule a callback to fire after \p delay_ns nanoseconds.
    ///
    /// Non-positive delays are clamped to one fine-bucket interval.
    ///
    /// \param[in] delay_ns Relative delay from the last advance time.
    /// \param[in] cb Callback to invoke when the timer fires.
    /// \return A unique timer identifier usable with cancel().
    [[nodiscard]] uint64_t schedule(int64_t delay_ns, TimerCallback cb);

    /// \brief Schedule a callback to fire at an absolute nanosecond deadline.
    ///
    /// \param[in] expire_ns Absolute deadline (monotonic nanoseconds).
    /// \param[in] cb Callback to invoke when the timer fires.
    /// \return A unique timer identifier usable with cancel().
    [[nodiscard]] uint64_t schedule_at(int64_t expire_ns, TimerCallback cb);

    /// \brief Cancel a pending timer.
    ///
    /// \param[in] timer_id Identifier returned by schedule() or schedule_at().
    /// \retval true The timer was found and cancelled.
    /// \retval false The timer was not found (already fired or cancelled).
    bool cancel(uint64_t timer_id);

    /// \brief Advance time and fire all timers whose deadline has elapsed.
    ///
    /// Processes up to max_advance_buckets fine-wheel buckets per call.
    /// Cascades from coarse to fine and remote to coarse on wheel wrap.
    ///
    /// \param[in] now_ns Current monotonic time in nanoseconds.
    /// \return The number of timer callbacks that were invoked.
    uint32_t advance(int64_t now_ns);

    /// \brief Check whether the queue has no pending timers.
    ///
    /// \return true if the queue is empty.
    bool empty() const;

    /// \brief Number of pending timers in the queue.
    ///
    /// \return The count of timers that have not yet fired or been cancelled.
    size_t size() const {
        return timer_map_.size();
    }

  private:
    /// \brief A single timer entry in the wheel.
    struct Timer {
        int64_t expire_ns;
        uint64_t id;
        TimerCallback callback;
        Timer* next = nullptr;
        Timer* prev = nullptr;
        uint32_t bucket_idx = 0;
        /// 0=fine, 1=coarse, 2=remote
        uint8_t wheel_level = 0;
    };

    /// \brief Doubly-linked list of timers within a single wheel bucket.
    struct BucketList {
        Timer* head = nullptr;
        Timer* tail = nullptr;
        uint32_t count = 0;

        /// \brief Append a timer to the end of the bucket list.
        void push_back(Timer* t);

        /// \brief Remove a timer from the bucket list.
        void unlink(Timer* t);
    };

    void insert_timer(Timer* timer, int64_t now_ns);
    void cascade_coarse(int64_t now_ns);
    void cascade_remote(int64_t now_ns);

    std::vector<BucketList> fine_wheel_;
    std::vector<BucketList> coarse_wheel_;
    std::vector<BucketList> remote_wheel_;
    std::unordered_map<uint64_t, Timer*> timer_map_;

    int64_t fine_bucket_ns_;
    int64_t coarse_bucket_ns_;
    int64_t remote_bucket_ns_;
    uint32_t fine_mask_;
    uint32_t coarse_mask_;
    uint32_t remote_mask_;
    uint32_t max_advance_buckets_;

    uint32_t current_fine_ = 0;
    uint32_t current_coarse_ = 0;
    uint32_t current_remote_ = 0;
    int64_t last_advance_ns_ = 0;

    std::atomic<uint64_t> next_id_{1};
    mutable std::recursive_mutex mutex_;
};

} // namespace hpactor::adt
