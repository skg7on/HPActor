// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/timer/timer_plane.hpp>

#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace hpactor::sched {

/// \brief Tracks a set of timer handles for bulk cancellation.
///
/// Used for actor lifecycle: when an actor stops, cancel_all()
/// is called to cancel all pending timers owned by that actor.
///
/// Thread-safe: all public methods acquire an internal mutex.
class TimerGroup {
  public:
    /// \brief Register a timer handle with this group.
    /// \param[in] handle Raw timer handle from TimerPlane::schedule().
    void add(uint64_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        handles_.insert(handle);
    }

    /// \brief Remove a timer handle from this group.
    ///
    /// Safe to call with handles that were not added (no-op).
    /// \param[in] handle Raw timer handle to remove.
    void remove(uint64_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        handles_.erase(handle);
    }

    /// \brief Cancel all registered timers via \p plane and clear the group.
    /// \param[in,out] plane TimerPlane used to cancel each handle.
    /// \return Number of timers successfully cancelled.
    size_t cancel_all(TimerPlane& plane) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (auto h : handles_) {
            if (plane.cancel(h))
                ++count;
        }
        handles_.clear();
        return count;
    }

    /// \brief Number of handles currently tracked.
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return handles_.size();
    }

    /// \brief Returns true if no handles are tracked.
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return handles_.empty();
    }

  private:
    std::unordered_set<uint64_t> handles_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::sched
