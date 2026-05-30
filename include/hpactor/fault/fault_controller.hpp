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
#include <hpactor/log/log_manager.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor::fault {

struct FaultControllerSnapshot {
    bool enabled;
    std::string active_scope;
    uint64_t replay_seed;
    size_t schedule_entry_count;
    uint64_t domain_ticks[14];
    uint64_t faults_fired;
};

class FaultController {
  public:
    FaultController();

    void load(const FaultSchedule& schedule);
    void clear();

    void enable(std::string_view scope_pattern);
    void disable();
    bool is_enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    bool check(std::string_view path,
               std::optional<ActorId> target = std::nullopt);

    void advance_tick(FaultDomain domain);

    void stall(FaultDomain domain, uint64_t delay_ticks);

    void set_replay_seed(uint64_t seed) { replay_seed_ = seed; }
    uint64_t replay_seed() const noexcept { return replay_seed_; }

    void set_log_manager(log::LogManager* lm) { log_manager_ = lm; }

    FaultControllerSnapshot snapshot() const;
    static FaultControllerSnapshot aggregate_snapshot();

    uint64_t faults_fired() const noexcept { return faults_fired_.load(std::memory_order_acquire); }

    void install();
    void remove();

    static FaultController* instance();

  private:
    static thread_local FaultController* tls_instance_;

    struct InstanceList {
        std::mutex mutex;
        std::vector<FaultController*> instances;
    };
    static InstanceList& instance_list();

    void load_impl(const FaultSchedule& schedule);
    void clear_impl();
    void enable_impl(std::string_view scope_pattern);
    void disable_impl();

    std::atomic<bool> enabled_{false};
    std::string active_scope_;
    // Shared pointer to allow check() to hold a stable snapshot while
    // load()/clear() swap in a new schedule from the broadcast path.
    std::shared_ptr<const FaultSchedule> schedule_;
    // Per-instance cursor, only accessed by the owning thread's check().
    size_t schedule_cursor_{0};
    uint64_t domain_ticks_[14]{};
    uint64_t replay_seed_{0};
    std::atomic<uint64_t> faults_fired_{0};
    log::LogManager* log_manager_ = nullptr;
};

} // namespace hpactor::fault
