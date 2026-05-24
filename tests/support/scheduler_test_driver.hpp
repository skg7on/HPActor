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

// Header-only SchedulerTestDriver for deterministic test pumping.

#pragma once

#include <hpactor/core/actor_system.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <cassert>
#include <cstddef>

namespace hpactor::test {

class SchedulerTestDriver {
  public:
    explicit SchedulerTestDriver(ActorSystem& system)
        : scheduler_(system.scheduler()) {
        assert(scheduler_ != nullptr);
        scheduler_->pause_workers();
    }

    ~SchedulerTestDriver() {
        if (scheduler_) {
            scheduler_->resume_workers();
        }
    }

    SchedulerTestDriver(const SchedulerTestDriver&) = delete;
    SchedulerTestDriver& operator=(const SchedulerTestDriver&) = delete;

    bool run_one() {
        return scheduler_->run_one_ready();
    }

    sched::SchedulerDrainResult drain(size_t max_items = 10'000) {
        return scheduler_->drain_ready(max_items);
    }

    template <typename Predicate>
    bool drain_until(Predicate&& predicate, size_t max_items = 10'000) {
        if (predicate()) {
            return true;
        }
        for (size_t i = 0; i < max_items; ++i) {
            if (!scheduler_->run_one_ready()) {
                return predicate();
            }
            if (predicate()) {
                return true;
            }
        }
        return predicate();
    }

  private:
    sched::IScheduler* scheduler_;
};

} // namespace hpactor::test