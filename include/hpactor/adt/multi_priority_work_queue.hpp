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

template <typename T> class MultiPriorityWorkQueue {
  public:
    explicit MultiPriorityWorkQueue(uint32_t priority_levels = 4)
        : levels_(priority_levels) {}

    void push(uint8_t priority, T item) {
        levels_[priority].push_bottom(std::move(item));
    }

    bool pop(T& out) {
        for (uint32_t i = 0; i < levels_.size(); ++i) {
            if (levels_[i].pop_bottom(out)) {
                return true;
            }
        }
        return false;
    }

    bool steal(T& out) {
        for (uint32_t i = 0; i < levels_.size(); ++i) {
            if (levels_[i].steal_top(out)) {
                return true;
            }
        }
        return false;
    }

    size_t depth_approx() const {
        size_t total = 0;
        for (const auto& level : levels_) {
            total += level.size_approx();
        }
        return total;
    }

    uint32_t num_levels() const {
        return static_cast<uint32_t>(levels_.size());
    }

  private:
    std::vector<ChaselevDeque<T>> levels_;
};

} // namespace hpactor::adt
