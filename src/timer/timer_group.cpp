// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/timer/timer_group.hpp>
#include <hpactor/timer/timer_plane.hpp>

namespace hpactor::sched {

void TimerGroup::add(uint64_t handle) {
    handles_.insert(handle);
}

void TimerGroup::remove(uint64_t handle) {
    handles_.erase(handle);
}

size_t TimerGroup::cancel_all(TimerPlane& plane) {
    size_t count = 0;
    for (auto h : handles_) {
        if (plane.cancel(h)) {
            ++count;
        }
    }
    handles_.clear();
    return count;
}

size_t TimerGroup::size() const {
    return handles_.size();
}

bool TimerGroup::empty() const {
    return handles_.empty();
}

} // namespace hpactor::sched
