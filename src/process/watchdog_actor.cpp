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
#include <hpactor/core/actor_system.hpp>
#include <hpactor/process/process_manager.hpp>
#include <hpactor/process/watchdog_actor.hpp>
#include <hpactor/sched/scheduler.hpp>

namespace hpactor::process {

WatchdogActor::WatchdogActor(ActorContext* ctx, ActorSystem& system,
                             std::chrono::milliseconds interval)
    : EventBasedActor(ctx, system), interval_(interval), system_(system) {}

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

bool WatchdogActor::is_system_healthy() const {
    (void)system_;
    return true;
}

} // namespace hpactor::process
