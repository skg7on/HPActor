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

#include <hpactor/fault/fault_schedule.hpp>
#include <hpactor/fault/fault_types.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hpactor::fault {

struct FaultControllerSnapshot {
    bool enabled;
    std::string active_scope;
    uint64_t replay_seed;
    size_t schedule_entry_count;
    uint64_t domain_ticks[9];
    uint64_t faults_fired;
};

class FaultController {
  public:
    FaultController();

    void load(const FaultSchedule& schedule);
    void clear();

    void enable(std::string_view scope_pattern);
    void disable(std::string_view scope_pattern);
    bool is_enabled() const noexcept { return enabled_; }

    bool check(std::string_view path,
               std::optional<ActorId> target = std::nullopt);

    void advance_tick(FaultDomain domain);

    void stall(FaultDomain domain, uint64_t delay_ticks);

    void set_replay_seed(uint64_t seed) { replay_seed_ = seed; }
    uint64_t replay_seed() const noexcept { return replay_seed_; }

    FaultControllerSnapshot snapshot() const;

    uint64_t faults_fired() const noexcept { return faults_fired_; }

    void install();
    void remove();

    static FaultController* instance() {
        return instance_;
    }

  private:
    static FaultController* instance_;

    bool enabled_;
    std::string active_scope_;
    FaultSchedule schedule_;
    size_t schedule_cursor_;
    uint64_t domain_ticks_[9];
    uint64_t replay_seed_;
    uint64_t faults_fired_;
};

} // namespace hpactor::fault
