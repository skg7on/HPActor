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

#include <chrono>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/process/health_check.hpp>
#include <hpactor/process/process_config.hpp>

namespace hpactor {

class ActorSystem;

namespace process {

struct WatchdogCheck {};

/// \brief Periodic system health-check actor for systemd watchdog integration.
///
/// On each timer tick the actor runs a set of composable health checks via
/// \c HealthCheckEngine and publishes the outcome to a shared \c HealthState.
/// When the overall status is \c Healthy or \c Degraded the actor notifies
/// systemd's watchdog (\c WATCHDOG=1).  When the status is \c Unhealthy
/// the notification is withheld so systemd restarts the process.
class WatchdogActor : public EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "WatchdogActor";

    /// \brief Construct a watchdog actor.
    /// \param ctx     Actor execution context.
    /// \param system  The actor system to monitor.
    /// \param interval  Period between consecutive health checks.
    /// \param health_config  Configuration for individual health checks.
    ///                       When \c enabled is \c false the watchdog always
    ///                       reports healthy.
    WatchdogActor(ActorContext* ctx, ActorSystem& system,
                  std::chrono::milliseconds interval,
                  const HealthCheckConfig& health_config = {});

    Behavior make_behavior() override;
    bool is_system_actor() const override {
        return true;
    }

    /// \brief Shared health state, updated on every check cycle.
    ///
    /// Other actors (e.g. \c HealthHttpServer) can read this to serve
    /// health endpoints without running independent checks.
    std::shared_ptr<HealthState> health_state() const {
        return health_state_;
    }

  private:
    void on_check();
    bool is_system_healthy();

    std::chrono::milliseconds interval_;
    ActorSystem& system_;
    std::unique_ptr<HealthCheckEngine> health_engine_;
    std::shared_ptr<HealthState> health_state_;
    HealthCheckConfig health_config_;
    std::chrono::steady_clock::time_point last_check_time_{};
};

} // namespace process
} // namespace hpactor
