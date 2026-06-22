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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::apps::cluster_control_plane {

// =============================================================================
// Scenario kinds (mirrors messages.hpp enum for the scenario runner layer)
// =============================================================================

enum class ScenarioKind : uint8_t;

// =============================================================================
// Configuration
// =============================================================================

struct ScenarioRunConfig {
    ScenarioKind scenario;
    uint32_t num_nodes = 3;
    uint32_t num_shards = 6;
    uint32_t num_tenants = 4;
    uint32_t requests_per_tenant = 10;
    bool enable_durable_state = true;
    bool enable_reliable_delivery = true;
    uint32_t scheduler_threads = 1;
    bool scheduler_start_paused = true;
};

ScenarioRunConfig default_scenario_config(ScenarioKind kind);

// =============================================================================
// Summary
// =============================================================================

struct ScenarioSummary {
    std::string scenario_name;
    std::string status;
    uint32_t nodes_alive = 0;
    uint32_t nodes_down = 0;
    uint32_t shards_total = 0;
    uint32_t shards_rebalanced_count = 0;
    std::string singleton_owner;
    uint64_t fencing_token = 0;
    uint32_t requests_sent = 0;
    uint32_t requests_allowed = 0;
    uint32_t requests_denied = 0;
    uint32_t policy_updates_acked = 0;
    uint32_t policy_updates_nacked = 0;
    uint32_t reliable_expired = 0;
    uint32_t dlq_depth = 0;
    uint32_t dlq_total_pushed = 0;
    uint32_t events_persisted = 0;
    uint32_t snapshots_taken = 0;
    bool partition_blocked = false;
    bool state_recovered = false;
    uint32_t actor_count = 0;
    uint64_t elapsed_ms = 0;

    void print() const;
};

// =============================================================================
// Scenario runner (declaration)
// =============================================================================

/// \brief Run a cluster scenario inside a single-process ActorSystem.
///
/// Creates an ActorSystem with the given config, builds the cluster topology,
/// spawns all demo actors, injects the scenario, drains the scheduler, and
/// returns a structured summary.
ScenarioSummary run_scenario(const ScenarioRunConfig& config);

// =============================================================================
// Helpers for distributed role modes
// =============================================================================

enum class NodeRole : uint8_t {
    Orchestrator,
    Gateway,
};

/// \brief Build config for all-in-one scenario mode.
hpactor::Config make_runtime_config(const ScenarioRunConfig& config);

/// \brief Build config for distributed role mode.
hpactor::Config make_role_config(const std::string& host, uint16_t port);

/// \brief Spawn role-specific actors for a distributed deployment.
void spawn_role_actors(ActorSystem& system, NodeRole role);

} // namespace hpactor::apps::cluster_control_plane
