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

#include <apps/edgeops_telemetry/messages.hpp>

#include <cstdint>
#include <string>

namespace hpactor {
class ActorSystem;
}

namespace hpactor::apps::edgeops_telemetry {

enum class RoleKind : uint8_t {
    Gateway = 0,
    Processor,
    Storage,
    Ops,
};

struct ScenarioRunConfig {
    ScenarioKind scenario = ScenarioKind::HappyPath;
    uint32_t device_count = 4;
    uint32_t readings_per_device = 3;
    uint32_t rate_per_second = 0;
    uint32_t storage_capacity = 64;
    bool enable_cli = false;
};

struct ScenarioSummary {
    ScenarioKind scenario = ScenarioKind::HappyPath;
    std::string status = "not-started";
    uint32_t devices_registered = 0;
    uint32_t devices_disconnected = 0;
    uint32_t devices_reconnected = 0;
    uint32_t readings_received = 0;
    uint32_t readings_normalized = 0;
    uint32_t readings_rejected = 0;
    uint32_t readings_stored = 0;
    uint32_t readings_dropped = 0;
    uint32_t rollups_emitted = 0;
    uint32_t alerts_raised = 0;
    uint32_t storage_peak_depth = 0;
    uint32_t storage_capacity = 0;
    uint32_t dlq_depth = 0;
    uint32_t dlq_total_pushed = 0;
    uint32_t dlq_total_lost = 0;
    uint32_t actor_count = 0;
    uint32_t scheduler_workers = 0;
    uint64_t elapsed_ms = 0;
    bool drained = false;
};

inline FleetSummaryPayload to_fleet_summary(const ScenarioSummary& summary) {
    FleetSummaryPayload fleet;
    fleet.devices_registered = summary.devices_registered;
    fleet.devices_disconnected = summary.devices_disconnected;
    fleet.readings_received = summary.readings_received;
    fleet.readings_normalized = summary.readings_normalized;
    fleet.readings_rejected = summary.readings_rejected;
    fleet.readings_stored = summary.readings_stored;
    fleet.readings_dropped = summary.readings_dropped;
    fleet.rollups_emitted = summary.rollups_emitted;
    fleet.alerts_raised = summary.alerts_raised;
    return fleet;
}

inline ScenarioRunConfig default_scenario_config(ScenarioKind scenario) {
    ScenarioRunConfig config;
    config.scenario = scenario;
    switch (scenario) {
        case ScenarioKind::Overload:
            config.device_count = 8;
            config.readings_per_device = 4;
            config.storage_capacity = 8;
            break;
        case ScenarioKind::DeviceChurn:
            config.device_count = 6;
            config.readings_per_device = 2;
            config.storage_capacity = 32;
            break;
        case ScenarioKind::TimerRollup:
            config.device_count = 3;
            config.readings_per_device = 4;
            config.storage_capacity = 32;
            break;
        case ScenarioKind::MalformedTelemetry:
        case ScenarioKind::MissingRoute:
        case ScenarioKind::ProcessorRestart:
        case ScenarioKind::GracefulShutdown:
        case ScenarioKind::FaultInjection:
        case ScenarioKind::HappyPath:
            break;
    }
    return config;
}

ScenarioSummary run_scenario(const ScenarioRunConfig& config);

/// Spawn role-specific actors into an existing ActorSystem.
void spawn_role_actors(hpactor::ActorSystem& system, RoleKind role,
                       uint32_t storage_capacity = 64);

} // namespace hpactor::apps::edgeops_telemetry