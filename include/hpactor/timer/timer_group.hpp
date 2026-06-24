// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <unordered_set>

namespace hpactor::sched {

class TimerPlane; // forward declaration

/// \brief Tracks a set of timer handles for bulk cancellation.
///
/// Used for actor lifecycle: when an actor stops, cancel_all()
/// is called to cancel all pending timers owned by that actor.
class TimerGroup {
  public:
    /// \brief Register a timer handle with this group.
    /// \param[in] handle Raw timer handle from TimerPlane::schedule().
    void add(uint64_t handle);

    /// \brief Remove a timer handle from this group.
    ///
    /// Safe to call with handles that were not added (no-op).
    /// \param[in] handle Raw timer handle to remove.
    void remove(uint64_t handle);

    /// \brief Cancel all registered timers via \p plane and clear the group.
    /// \param[in,out] plane TimerPlane used to cancel each handle.
    /// \return Number of timers successfully cancelled.
    size_t cancel_all(TimerPlane& plane);

    /// \brief Number of handles currently tracked.
    size_t size() const;

    /// \brief Returns true if no handles are tracked.
    bool empty() const;

  private:
    std::unordered_set<uint64_t> handles_;
};

} // namespace hpactor::sched
