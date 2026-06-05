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

#include <hpactor/actor/shutdown_coordinator.hpp>

namespace hpactor {

ShutdownCoordinator::ShutdownCoordinator(ShutdownCoordinatorDependencies deps)
    : deps_(std::move(deps)) {}

void ShutdownCoordinator::set_phase(ShutdownPhase phase) noexcept {
    if (deps_.phase) {
        deps_.phase->store(phase, std::memory_order_release);
    }
}

ShutdownPhase ShutdownCoordinator::phase() const noexcept {
    if (deps_.phase) {
        return deps_.phase->load(std::memory_order_acquire);
    }
    return ShutdownPhase::Running;
}

bool ShutdownCoordinator::accepting_ingress() const noexcept {
    auto current = phase();
    return current == ShutdownPhase::Running;
}

void ShutdownCoordinator::shutdown(std::chrono::milliseconds drain_timeout) {
    (void)drain_timeout;
    set_phase(ShutdownPhase::DrainingIngress);
    if (deps_.set_ready) {
        deps_.set_ready(false);
    }

    set_phase(ShutdownPhase::DrainingActors);
    for (auto id : deps_.actor_snapshot()) {
        deps_.request_actor_drain(id);
    }

    set_phase(ShutdownPhase::LeavingCluster);
    if (deps_.leave_discovery) {
        deps_.leave_discovery();
    }
    if (deps_.stop_remote_runtime) {
        deps_.stop_remote_runtime();
    }

    set_phase(ShutdownPhase::FlushingTelemetry);
    if (deps_.flush_telemetry) {
        deps_.flush_telemetry();
    }

    set_phase(ShutdownPhase::Stopped);
}

} // namespace hpactor
