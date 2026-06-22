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

#include <hpactor/cluster/cluster_node_state.hpp>

#include <functional>
#include <string>
#include <vector>

namespace hpactor::cluster {

/// \brief Whether transitioning to this state should trigger route
/// invalidation.
constexpr bool should_invalidate_routes(ClusterNodeState state) noexcept {
    return state == ClusterNodeState::Down ||
           state == ClusterNodeState::Quarantined ||
           state == ClusterNodeState::Removed;
}

/// \brief Orchestrates route invalidation across multiple subsystems.
///
/// When a node becomes unreachable for routing (Down, Quarantined, Removed),
/// this class coordinates the cleanup via registered callbacks.
class RouteInvalidation {
  public:
    RouteInvalidation() = default;

    void process(const std::vector<std::string>& node_ids);

    void register_callback(std::function<void(const std::string&)> callback);

  private:
    std::vector<std::function<void(const std::string&)>> callbacks_;
};

} // namespace hpactor::cluster
