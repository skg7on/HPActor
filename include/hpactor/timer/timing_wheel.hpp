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

/// \brief Hierarchical timer wheel for O(1) timer insert and cancel.
///
/// Modeled after the Linux kernel timer wheel.  The wheel has multiple
/// levels, each with a different granularity, enabling efficient
/// management of timers from 1 ms to hours:
///
/// | Level | Resolution | Buckets | Coverage       |
/// |-------|------------|---------|----------------|
/// | 0     | 1 ms       | 256     | 256 ms         |
/// | 1     | 256 ms     | 256     | ~65.5 s        |
/// | 2     | ~65.5 s    | 256     | ~4.66 hours    |
/// | 3     | ~4.66 h    | 256     | ~49.7 days     |
///
/// Higher-level buckets are cascaded down to lower levels as time
/// advances so that timers eventually reach level 0 and fire.
///
/// \note **Thread safety**: \c schedule(), \c schedule_at(), \c cancel(),
///       and \c advance() are safe to call from any thread.  All bucket
///       operations are serialized by an internal \c std::mutex.
///       Callbacks registered via \c schedule() are fired on the calling
///       thread of \c advance() **without** the internal mutex held, so
///       callbacks may safely call back into \c schedule() or \c cancel()
///       without deadlock (the callback acquires the mutex as a new owner;
///       recursion is not required).
///
/// \note **Advance cap**: \c advance() caps each time step to at most
///       100 ms to prevent a positive feedback loop where slow callback
///       execution causes the timer thread to fall behind, producing
///       larger time windows and exponentially more callbacks in
///       successive calls.
class TimingWheel {
  public:
    /// \brief Callback type invoked when a scheduled timer expires.
    ///
    /// \note Callbacks are fired on the thread that calls \c advance().
    using TimerCallback = std::function<void()>;

    /// \brief A single entry in the timing wheel.
    struct Timer : mem::SlabAllocated<Timer> {
        int64_t expire_ns;      ///< Absolute expiration time (monotonic
                                ///< clock, nanoseconds).
        uint64_t id;            ///< Unique timer identifier.
        TimerCallback callback; ///< Callback to invoke on expiry.
    };

    /// \brief Construct a timing wheel.
    /// \param[in] tick_ns Duration of one tick at level 0, in nanoseconds.
    ///                    Default is 1 ms (1'000'000 ns).
    /// \param[in] num_levels Number of hierarchical levels.  Default is 4.
    explicit TimingWheel(int64_t tick_ns = 1'000'000, uint32_t num_levels = 4);

    /// \brief Destructor — deletes all pending timers.
    ///
    /// \warning Pending timer callbacks are **not** invoked.  Callers
    ///          that need graceful cancellation should call \c cancel()
    ///          on every outstanding timer before destroying the wheel.
    ~TimingWheel();

    TimingWheel(const TimingWheel&) = delete;
    TimingWheel& operator=(const TimingWheel&) = delete;
    TimingWheel(TimingWheel&&) = delete;
    TimingWheel& operator=(TimingWheel&&) = delete;

    /// \brief Schedule a one-shot timer.
    ///
    /// \param[in] delay_ns Relative delay in nanoseconds from the wheel's
    ///                     current time (see \c current_time()).
    /// \param[in] callback Callback to invoke when the timer expires.
    /// \return A unique timer identifier suitable for \c cancel().
    ///         Returns 0 if the fault-injection site
    ///         \c hpactor.timing_wheel.schedule.fail fires.
    /// \note Thread-safe.  Acquires the internal mutex.
    uint64_t schedule(int64_t delay_ns, TimerCallback callback);

    /// \brief Schedule a one-shot timer at an absolute expiration time.
    ///
    /// \param[in] expire_ns Absolute expiration time in nanoseconds
    ///                      (monotonic clock).
    /// \param[in] callback Callback to invoke when the timer expires.
    /// \return A unique timer identifier suitable for \c cancel().
    /// \note Thread-safe.  Acquires the internal mutex.
    uint64_t schedule_at(int64_t expire_ns, TimerCallback callback);

    /// \brief Cancel and destroy a previously scheduled timer.
    ///
    /// The timer is removed from the wheel and its memory is freed.
    /// Its callback is **not** invoked.
    ///
    /// \param[in] timer_id Timer identifier returned by \c schedule() or
    ///                     \c schedule_at().
    /// \retval true  Timer was found, removed, and deleted.
    /// \retval false Timer was not found (already fired, cancelled, or
    ///               invalid \p timer_id).
    /// \note Thread-safe.  Acquires the internal mutex.
    bool cancel(uint64_t timer_id);

    /// \brief Advance the wheel to the given absolute time.
    ///
    /// Processes all buckets that have become due between the last
    /// advance time and \p now_ns.  Expired timers are collected under
    /// the internal mutex; their callbacks are invoked **after** the
    /// mutex is released so that callbacks may safely call \c schedule()
    /// or \c cancel() without blocking other threads on the mutex.
    ///
    /// Each advance step is capped to at most 100 ms.  If \p now_ns
    /// exceeds the last advance time by more than 100 ms the wheel
    /// advances by exactly 100 ms; the remaining time is covered by
    /// subsequent calls.
    ///
    /// \param[in] now_ns Current absolute time in nanoseconds (monotonic
    ///                   clock).  Must be greater than the last \p now_ns
    ///                   passed to this function.  Capped internally at
    ///                   \c old_time + 100ms.
    /// \return The number of timer callbacks that were invoked.
    /// \note Thread-safe.  Acquires the internal mutex only while
    ///       collecting expired timers; releases it before firing
    ///       callbacks.
    uint32_t advance(int64_t now_ns);

    /// \brief Current time according to the wheel.
    ///
    /// Updated by \c advance().  Used as the base for relative delays
    /// passed to \c schedule().
    ///
    /// \return The last \p now_ns value (capped) passed to \c advance().
    /// \note Lock-free.  Safe to call from any thread.
    int64_t current_time() const {
        return current_time_.load(std::memory_order_relaxed);
    }

    /// \brief Check whether the wheel has no pending timers.
    ///
    /// \retval true  All buckets at every level are empty.
    /// \retval false At least one pending timer exists.
    /// \note Thread-safe.  Acquires the internal mutex for a consistent
    ///       snapshot.  The result may be stale by the time the caller
    ///       observes it.
    bool empty() const;

    /// \brief Return the earliest timer expiration time, or INT64_MAX if empty.
    ///
    /// \return The minimum expiration time of all pending timers, or
    ///         \c INT64_MAX if no timers are pending.
    /// \note Thread-safe.  Acquires the internal mutex for a consistent
    ///       snapshot.  The result may be stale by the time the caller
    ///       observes it.
    int64_t next_deadline() const;

    /// \brief Number of pending timers.
    ///
    /// Sums the size of every bucket at every level.
    ///
    /// \return Total pending timer count across all buckets.
    /// \note Thread-safe.  Acquires the internal mutex for a consistent
    ///       snapshot.  The count may be stale immediately after the
    ///       call returns.
    size_t size() const;

  private:
    struct WheelLevel {
        std::vector<std::vector<Timer*>> buckets;
        uint32_t num_buckets;
        uint32_t mask;
    };

    uint64_t add_timer_internal(int64_t expire_ns, TimerCallback callback);
    void insert_timer(Timer* timer);
    Timer* remove_timer(uint64_t timer_id);

    void recompute_min_deadline();

    int64_t tick_ns_;
    uint32_t num_levels_;
    std::vector<WheelLevel> levels_;
    std::vector<int64_t> level_ranges_; ///< Precomputed range per level.

    std::atomic<int64_t> current_time_{0};
    uint64_t next_timer_id_{1};

    /// Cached minimum deadline across all pending timers.
    /// Updated under mutex_ during schedule(), advance(), cancel().
    /// Read lock-free by next_deadline().
    std::atomic<int64_t> min_deadline_{INT64_MAX};

    /// \brief Protects all bucket operations.
    ///
    /// \c schedule(), \c cancel(), \c advance(), \c empty(), \c size(),
    /// and the destructor acquire this mutex.  Callbacks registered via
    /// \c schedule() are fired by \c advance() **outside** the lock, so
    /// a callback that calls back into \c schedule() or \c cancel()
    /// acquires the mutex as a new owner — recursion is not required.
    mutable std::mutex mutex_;
};

} // namespace hpactor::sched