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

#include <hpactor/cluster/sharding/rendezvous_hash.hpp>

#include <functional>
#include <limits>

namespace hpactor::cluster::sharding {

PlacementPlan
RendezvousHash::plan(std::span<const ShardId> shards,
                     std::span<const std::string> alive_nodes,
                     std::span<const ShardEntry> /*current_assignments*/) {
    PlacementPlan result;
    if (alive_nodes.empty())
        return result;

    for (auto shard : shards) {
        uint64_t max_weight = 0;
        std::string best_node;

        for (const auto& node : alive_nodes) {
            // Hash(shard_id, node) to get a weight — highest wins
            std::string combined = std::to_string(shard) + ":" + node;
            uint64_t weight = std::hash<std::string>{}(combined);
            if (weight > max_weight) {
                max_weight = weight;
                best_node = node;
            }
        }

        result.assignments[shard] = best_node;
    }
    return result;
}

} // namespace hpactor::cluster::sharding
