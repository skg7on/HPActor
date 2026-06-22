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

#include <hpactor/cluster/sharding/static_placement.hpp>

#include <algorithm>

namespace hpactor::cluster::sharding {

void StaticPlacement::set_mapping(ShardId shard, const std::string& node) {
    mapping_[shard] = node;
}

PlacementPlan
StaticPlacement::plan(std::span<const ShardId> shards,
                      std::span<const std::string> alive_nodes,
                      std::span<const ShardEntry> /*current_assignments*/) {
    PlacementPlan result;
    for (auto shard : shards) {
        auto it = mapping_.find(shard);
        if (it == mapping_.end())
            continue; // no static mapping for this shard

        // Only assign if the configured owner is alive
        bool owner_alive = false;
        for (const auto& node : alive_nodes) {
            if (node == it->second) {
                owner_alive = true;
                break;
            }
        }
        if (owner_alive) {
            result.assignments[shard] = it->second;
        }
    }
    return result;
}

} // namespace hpactor::cluster::sharding
