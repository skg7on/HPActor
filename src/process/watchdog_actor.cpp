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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/process/health_check.hpp>
#include <hpactor/process/process_config.hpp>
#include <hpactor/process/process_manager.hpp>
#include <hpactor/process/watchdog_actor.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <string>

namespace hpactor::process {

WatchdogActor::WatchdogActor(ActorContext* ctx, ActorSystem& system,
                             std::chrono::milliseconds interval,
                             const HealthCheckConfig& health_config)
    : EventBasedActor(ctx, system), interval_(interval), system_(system),
      health_engine_(make_health_check_engine(health_config)),
      health_state_(std::make_shared<HealthState>()),
      health_config_(health_config) {}

Behavior WatchdogActor::make_behavior() {
    // Schedule the first watchdog check via scheduler timer
    auto* sched = get_scheduler();
    if (sched) {
        auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(interval_).count();
        std::weak_ptr<AbstractActor> weak_self = shared_from_this();
        sched->schedule_after(
            [weak_self]() {
                if (auto self = weak_self.lock()) {
                    auto* actor = static_cast<WatchdogActor*>(self.get());
                    actor->on_check();
                }
            },
            ns);
    }
    return Behavior{};
}

void WatchdogActor::on_check() {
    if (is_system_healthy()) {
        ProcessManager::notify_watchdog();
    }
    // When unhealthy the watchdog intentionally does NOT notify systemd.
    // systemd will kill the process after WatchdogSec expires.

    // Reschedule for the next interval
    auto* sched = get_scheduler();
    if (sched) {
        auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(interval_).count();
        std::weak_ptr<AbstractActor> weak_self = shared_from_this();
        sched->schedule_after(
            [weak_self]() {
                if (auto self = weak_self.lock()) {
                    auto* actor = static_cast<WatchdogActor*>(self.get());
                    actor->on_check();
                }
            },
            ns);
    }
}

bool WatchdogActor::is_system_healthy() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_check_time_);

    CheckContext ctx{system_, elapsed};
    health_engine_->run_all(ctx, *health_state_);
    last_check_time_ = now;

    HealthStatus status = health_state_->overall_status();

    if (status == HealthStatus::Unhealthy) {
        // Build a status string for systemd's STATUS= notification.
        std::string status_msg = "UNHEALTHY: ";
        const auto& details = health_state_->details();
        bool first = true;
        for (const auto& d : details) {
            if (d.status != HealthStatus::Healthy) {
                if (!first)
                    status_msg += "; ";
                first = false;
                status_msg += d.check_name;
                status_msg += "=";
                status_msg += d.reason;
            }
        }
        ProcessManager::notify_status(status_msg);
        return false;
    }

    return true;
}

} // namespace hpactor::process
