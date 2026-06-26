// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/sched/scheduler_interfaces.hpp>
#include <hpactor/timer/timer_plane_shard.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace hpactor::sched {

/// \brief Sharded, actor-aware high-performance timer backend.
///
/// One shard per scheduler worker (or caller thread).  Each shard has
/// its own wheel, slot array, and mutex — contention is proportional
/// to cross-thread schedule/cancel operations.
///
/// TimerPlane is a passive backend: the scheduler's existing timer
/// thread drives advance() and reads next_deadline() via std::visit.
class TimerPlane {
  public:
    /// \brief Construct the TimerPlane.
    /// \param[in] num_shards Number of shards (typically worker_count).
    /// \param[in] tick_ns Wheel tick granularity (default 1 ms).
    explicit TimerPlane(uint32_t num_shards, int64_t tick_ns = 1'000'000);

    // ── Backend API (compatible with TimingWheel / CalendarQueue) ──────────

    /// \brief Schedule a one-shot timer.  Returns raw timer ID.
    /// \param[in] delay_ns Delay in nanoseconds from now.
    /// \param[in] cb Callback invoked when the timer fires.
    /// \return Timer identifier, or 0 on failure.
    uint64_t schedule(int64_t delay_ns, timer_callback cb);

    /// \brief Cancel a timer by raw ID.
    /// \param[in] timer_id Timer identifier from schedule().
    /// \retval true Timer was found and cancelled.
    /// \retval false Timer was not found (already fired or invalid ID).
    bool cancel(uint64_t timer_id);

    /// \brief Advance all shards to \p now_ns, firing expired timers.
    /// \param[in] now_ns Current time in nanoseconds.
    /// \return Total number of timers fired across all shards.
    uint32_t advance(int64_t now_ns);

    // ── Queries ────────────────────────────────────────────────────────────

    /// \brief Earliest deadline across all shards, or INT64_MAX if empty.
    int64_t next_deadline() const;

    /// \brief Returns true if no timers are pending across all shards.
    bool empty() const;

    /// \brief Total pending timer count across all shards.
    size_t size() const;

    // ── Shard access (for metrics / CLI) ───────────────────────────────────

    /// \brief Const access to a specific shard for metrics or debugging.
    /// \param[in] i Shard index (0 to num_shards() - 1).
    /// \return Const reference to the shard.
    const TimerPlaneShard& shard(uint32_t i) const;

    /// \brief Mutable access to a specific shard.
    /// \param[in] i Shard index (0 to num_shards() - 1).
    /// \return Mutable reference to the shard.
    TimerPlaneShard& shard(uint32_t i);

    /// \brief Number of shards.
    uint32_t num_shards() const;

  private:
    /// \brief Hash the current thread ID to select a shard.
    ///
    /// Same-thread schedules hit the same shard, minimizing cross-thread
    /// traffic for actor self-timers.
    uint32_t select_shard() const;

    std::vector<std::unique_ptr<TimerPlaneShard>> shards_;
    uint32_t num_shards_;
};

} // namespace hpactor::sched
