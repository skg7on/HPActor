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

#include <atomic>
#include <chrono>
#include <functional>
#include <hpactor/actor/shutdown_phase.hpp>
#include <hpactor/types/types.hpp>

#include <memory>
#include <vector>

namespace hpactor {

struct ShutdownCoordinatorDependencies {
    std::atomic<ShutdownPhase>* phase{nullptr};
    std::function<void(bool)> set_ready;
    std::function<std::vector<ActorId>()> actor_snapshot;
    std::function<void(ActorId)> request_actor_drain;
    std::function<bool()> actors_drained;
    std::function<void()> stop_remote_runtime;
    std::function<void()> leave_discovery;
    std::function<void()> flush_telemetry;
};

class ShutdownCoordinator {
  public:
    explicit ShutdownCoordinator(ShutdownCoordinatorDependencies deps);

    void shutdown(std::chrono::milliseconds drain_timeout);
    ShutdownPhase phase() const noexcept;
    bool accepting_ingress() const noexcept;

  private:
    void set_phase(ShutdownPhase phase) noexcept;
    ShutdownCoordinatorDependencies deps_;
};

} // namespace hpactor
