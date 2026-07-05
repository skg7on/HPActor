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

#include <hpactor/process/health_check.hpp>

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/process/process_config.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <algorithm>

#if defined(__linux__)
#    include <sys/sysinfo.h>
#elif defined(__APPLE__)
#    include <mach/mach.h>
#endif

namespace hpactor::process {

// =========================================================================
// HealthState
// =========================================================================

void HealthState::update(HealthStatus overall,
                         std::vector<HealthCheckResult> details,
                         std::chrono::steady_clock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    overall_ = overall;
    details_ = std::move(details);
    last_check_ = timestamp;
}

HealthStatus HealthState::overall_status() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return overall_;
}

std::vector<HealthCheckResult> HealthState::details() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return details_;
}

std::chrono::steady_clock::time_point HealthState::last_check_time() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_check_;
}

// =========================================================================
// HealthCheckEngine
// =========================================================================

void HealthCheckEngine::add_check(std::unique_ptr<IHealthCheck> check) {
    checks_.push_back(std::move(check));
}

void HealthCheckEngine::run_all(const CheckContext& ctx, HealthState& state) {
    std::vector<HealthCheckResult> details;
    details.reserve(checks_.size());

    HealthStatus overall = HealthStatus::Healthy;

    for (const auto& check : checks_) {
        auto result = check->check(ctx);
        HealthStatus s = result.status;

        if (s == HealthStatus::Unhealthy && check->is_critical()) {
            overall = HealthStatus::Unhealthy;
        } else if (s != HealthStatus::Healthy && overall == HealthStatus::Healthy) {
            overall = HealthStatus::Degraded;
        }

        details.push_back(std::move(result));
    }

    state.update(overall, std::move(details), std::chrono::steady_clock::now());
}

// =========================================================================
// SchedulerLivenessCheck
// =========================================================================

SchedulerLivenessCheck::SchedulerLivenessCheck(uint32_t progress_deadline_sec)
    : progress_deadline_sec_(progress_deadline_sec) {}

std::string_view SchedulerLivenessCheck::name() const noexcept {
    return "scheduler_liveness";
}

bool SchedulerLivenessCheck::is_critical() const noexcept {
    return true;
}

void SchedulerLivenessCheck::reset_baseline() noexcept {
    has_baseline_ = false;
}

HealthCheckResult SchedulerLivenessCheck::check(const CheckContext& ctx) {
    HealthCheckResult result;
    result.check_name = "scheduler_liveness";

    auto* sched = ctx.system.scheduler();
    if (!sched) {
        result.status = HealthStatus::Unhealthy;
        result.reason = "no scheduler available";
        return result;
    }

    if (!sched->is_running()) {
        result.status = HealthStatus::Unhealthy;
        result.reason = "scheduler is not running";
        return result;
    }

    // Sum actors_executed across all workers to detect forward progress.
    auto snapshots = sched->worker_snapshots();
    uint64_t total_executed = 0;
    for (const auto& ws : snapshots) {
        total_executed += ws.actors_executed;
    }

    if (!has_baseline_) {
        // First check — establish baseline.
        prev_actors_executed_ = total_executed;
        last_progress_time_ = std::chrono::steady_clock::now();
        has_baseline_ = true;
        result.status = HealthStatus::Healthy;
        return result;
    }

    if (total_executed > prev_actors_executed_) {
        // Forward progress detected.
        prev_actors_executed_ = total_executed;
        last_progress_time_ = std::chrono::steady_clock::now();
        result.status = HealthStatus::Healthy;
        return result;
    }

    // No progress since last check.
    auto now = std::chrono::steady_clock::now();
    auto stall_duration =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_progress_time_);

    if (stall_duration.count() >= static_cast<int64_t>(progress_deadline_sec_)) {
        result.status = HealthStatus::Unhealthy;
        result.reason =
            "no scheduler progress for " + std::to_string(stall_duration.count()) +
            "s (deadline: " + std::to_string(progress_deadline_sec_) + "s)";
    } else {
        result.status = HealthStatus::Healthy;
    }

    return result;
}

// =========================================================================
// SystemActorHealthCheck
// =========================================================================

std::string_view SystemActorHealthCheck::name() const noexcept {
    return "system_actor_health";
}

bool SystemActorHealthCheck::is_critical() const noexcept {
    return true;
}

HealthCheckResult SystemActorHealthCheck::check(const CheckContext& ctx) {
    HealthCheckResult result;
    result.check_name = "system_actor_health";

    bool any_failed = false;
    bool any_quarantined = false;
    bool any_passivating = false;

    ctx.system.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        if (!actor.is_system_actor())
            return;

        auto* lifecycle = actor.as_lifecycle();
        if (!lifecycle)
            return;

        auto state = lifecycle->state();
        if (state == LifecycleState::kFailed) {
            any_failed = true;
        } else if (state == LifecycleState::kQuarantined) {
            any_quarantined = true;
        } else if (state == LifecycleState::kPassivating ||
                   state == LifecycleState::kPassivated) {
            any_passivating = true;
        }
    });

    if (any_failed) {
        result.status = HealthStatus::Unhealthy;
        result.reason = "one or more system actors in Failed state";
    } else if (any_quarantined) {
        result.status = HealthStatus::Unhealthy;
        result.reason = "one or more system actors in Quarantined state";
    } else if (any_passivating) {
        result.status = HealthStatus::Degraded;
        result.reason = "one or more system actors are passivating";
    } else {
        result.status = HealthStatus::Healthy;
    }

    return result;
}

// =========================================================================
// DLQGrowthCheck
// =========================================================================

DLQGrowthCheck::DLQGrowthCheck(uint32_t depth_warning_pct,
                               uint32_t depth_critical_pct,
                               uint32_t lost_rate_per_minute)
    : depth_warning_pct_(depth_warning_pct),
      depth_critical_pct_(depth_critical_pct),
      lost_rate_per_minute_(lost_rate_per_minute) {}

std::string_view DLQGrowthCheck::name() const noexcept {
    return "dlq_growth";
}

bool DLQGrowthCheck::is_critical() const noexcept {
    return false;
}

void DLQGrowthCheck::reset_baseline() noexcept {
    has_baseline_ = false;
}

void DLQGrowthCheck::update_history(uint64_t total_lost) {
    prev_total_lost_ = total_lost;
    has_baseline_ = true;
}

HealthCheckResult DLQGrowthCheck::check(const CheckContext& ctx) {
    HealthCheckResult result;
    result.check_name = "dlq_growth";

    auto snapshot = ctx.system.dead_letter_snapshot();

    // Depth-based checks.
    if (snapshot.capacity > 0) {
        double depth_pct = 100.0 * static_cast<double>(snapshot.depth) /
                           static_cast<double>(snapshot.capacity);

        if (depth_pct >= static_cast<double>(depth_critical_pct_)) {
            result.status = HealthStatus::Unhealthy;
            result.reason = "DLQ at " +
                            std::to_string(static_cast<uint32_t>(depth_pct)) +
                            "% capacity (" + std::to_string(snapshot.depth) +
                            "/" + std::to_string(snapshot.capacity) + ")";
            update_history(snapshot.total_lost);
            return result;
        }

        if (depth_pct >= static_cast<double>(depth_warning_pct_)) {
            result.status = HealthStatus::Degraded;
            result.reason = "DLQ at " +
                            std::to_string(static_cast<uint32_t>(depth_pct)) +
                            "% capacity (" + std::to_string(snapshot.depth) +
                            "/" + std::to_string(snapshot.capacity) + ")";
            update_history(snapshot.total_lost);
            return result;
        }
    }

    // Loss rate check.
    if (has_baseline_ && ctx.elapsed.count() > 0) {
        uint64_t lost_delta = snapshot.total_lost - prev_total_lost_;
        double elapsed_min = static_cast<double>(ctx.elapsed.count()) / 60000.0;
        if (elapsed_min > 0.0) {
            double lost_rate = static_cast<double>(lost_delta) / elapsed_min;
            if (lost_rate >= static_cast<double>(lost_rate_per_minute_)) {
                result.status = HealthStatus::Degraded;
                result.reason = "DLQ loss rate " +
                                std::to_string(static_cast<uint32_t>(lost_rate)) +
                                "/min exceeds threshold " +
                                std::to_string(lost_rate_per_minute_) + "/min";
                update_history(snapshot.total_lost);
                return result;
            }
        }
    }

    update_history(snapshot.total_lost);
    result.status = HealthStatus::Healthy;
    return result;
}

// =========================================================================
// MemoryPressureCheck
// =========================================================================

namespace {

/// \brief Read OS-reported memory utilisation as a percentage (0-100).
static uint8_t read_os_memory_pressure() {
#if defined(__linux__)
    struct sysinfo si{};
    if (sysinfo(&si) != 0)
        return 0;
    if (si.totalram == 0)
        return 0;
    uint64_t used = si.totalram - si.freeram;
    uint64_t reclaimable = si.bufferram;
    if (used > reclaimable)
        used -= reclaimable;
    else
        used = 0;
    uint64_t pct = (used * 100ULL) / si.totalram;
    pct = std::min<uint64_t>(pct, 100ULL);
    return static_cast<uint8_t>(pct);
#elif defined(__APPLE__)
    mach_port_t host = mach_host_self();
    vm_size_t page_size = 0;
    host_page_size(host, &page_size);
    if (page_size == 0) {
        mach_port_deallocate(mach_task_self(), host);
        return 0;
    }
    vm_statistics64_data_t vm_stat{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    kern_return_t kr = host_statistics64(
        host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm_stat), &count);
    mach_port_deallocate(mach_task_self(), host);
    if (kr != KERN_SUCCESS)
        return 0;

    uint64_t used_pages = vm_stat.active_count + vm_stat.inactive_count +
                          vm_stat.wire_count + vm_stat.speculative_count +
                          vm_stat.compressor_page_count;
    uint64_t total_pages = used_pages + vm_stat.free_count;
    if (total_pages == 0)
        return 0;
    uint64_t pct = (used_pages * 100ULL) / total_pages;
    pct = std::min<uint64_t>(pct, 100ULL);
    return static_cast<uint8_t>(pct);
#else
    return 0;
#endif
}

} // anonymous namespace

MemoryPressureCheck::MemoryPressureCheck(uint8_t warning_pct, uint8_t critical_pct,
                                         PressureReader reader)
    : warning_pct_(warning_pct), critical_pct_(critical_pct),
      reader_(reader ? reader : &read_os_memory_pressure) {}

std::string_view MemoryPressureCheck::name() const noexcept {
    return "memory_pressure";
}

bool MemoryPressureCheck::is_critical() const noexcept {
    return true;
}

HealthCheckResult MemoryPressureCheck::check(const CheckContext& ctx) {
    (void)ctx;
    HealthCheckResult result;
    result.check_name = "memory_pressure";

    uint8_t pct = reader_();

    if (pct >= critical_pct_) {
        result.status = HealthStatus::Unhealthy;
        result.reason = "memory pressure " + std::to_string(pct) +
                        "% >= critical threshold " +
                        std::to_string(critical_pct_) + "%";
    } else if (pct >= warning_pct_) {
        result.status = HealthStatus::Degraded;
        result.reason = "memory pressure " + std::to_string(pct) +
                        "% >= warning threshold " +
                        std::to_string(warning_pct_) + "%";
    } else {
        result.status = HealthStatus::Healthy;
    }

    return result;
}

// =========================================================================
// Factory function
// =========================================================================

std::unique_ptr<HealthCheckEngine>
make_health_check_engine(const HealthCheckConfig& config) {
    auto engine = std::make_unique<HealthCheckEngine>();

    if (!config.enabled)
        return engine;

    if (config.scheduler_liveness_enabled) {
        engine->add_check(std::make_unique<SchedulerLivenessCheck>(
            config.scheduler_progress_deadline_sec));
    }

    if (config.system_actor_health_enabled) {
        engine->add_check(std::make_unique<SystemActorHealthCheck>());
    }

    if (config.dlq_growth_enabled) {
        engine->add_check(std::make_unique<DLQGrowthCheck>(
            config.dlq_depth_warning_pct, config.dlq_depth_critical_pct,
            config.dlq_lost_rate_per_minute));
    }

    if (config.memory_pressure_enabled) {
        engine->add_check(std::make_unique<MemoryPressureCheck>(
            config.memory_warning_pct, config.memory_critical_pct));
    }

    return engine;
}

} // namespace hpactor::process
