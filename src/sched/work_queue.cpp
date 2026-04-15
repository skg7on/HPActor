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

// src/sched/work_queue.cpp
#include <hpactor/sched/work_queue.hpp>

namespace hpactor::sched {

// --- MultiPriorityWorkQueue ---
MultiPriorityWorkQueue::MultiPriorityWorkQueue(uint32_t priority_levels)
    : levels_(priority_levels) {}

void MultiPriorityWorkQueue::push(uint8_t priority, WorkItem item) {
    levels_[priority].push_bottom(item);
}

bool MultiPriorityWorkQueue::pop(WorkItem& out) {
    // Scan from highest priority to lowest
    for (uint32_t i = 0; i < levels_.size(); ++i) {
        if (levels_[i].pop_bottom(out)) {
            return true;
        }
    }
    return false;
}

size_t MultiPriorityWorkQueue::depth_approx() const {
    size_t total = 0;
    for (const auto& level : levels_) {
        total += level.size_approx();
    }
    return total;
}

} // namespace hpactor::sched
