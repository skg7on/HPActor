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

#include <hpactor/cluster/sharding/shard_types.hpp>

#include <span>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::sharding {

/// \brief Result of a placement computation.
struct PlacementPlan {
    /// Map of shard_id → owner_node
    std::unordered_map<ShardId, std::string> assignments;
};

/// \brief Interface for shard placement strategies.
class IPlacementStrategy {
  public:
    virtual ~IPlacementStrategy() = default;

    /// \brief Compute shard→node assignment.
    ///
    /// \param[in] shards All shard IDs to place.
    /// \param[in] alive_nodes Currently alive nodes.
    /// \param[in] current_assignments Existing assignments (may be empty).
    /// \return PlacementPlan with assignments.
    virtual PlacementPlan
    plan(std::span<const ShardId> shards, std::span<const std::string> alive_nodes,
         std::span<const ShardEntry> current_assignments) = 0;
};

} // namespace hpactor::cluster::sharding
