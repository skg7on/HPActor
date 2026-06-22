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

#include <hpactor/cluster/sharding/shard_coordinator.hpp>
#include <hpactor/cluster/sharding/shard_resolver.hpp>

#include <memory>
#include <string>
#include <vector>

namespace hpactor::cluster::sharding {

/// \brief Actor wrapper around ShardCoordinatorCore.
class ShardCoordinatorActor {
  public:
    ShardCoordinatorActor(uint32_t total_shards,
                          std::unique_ptr<IPlacementStrategy> strategy)
        : core_(total_shards, std::move(strategy)) {}

    uint32_t total_shards() const {
        return core_.total_shards();
    }
    ShardCoordinatorCore& core() {
        return core_;
    }
    const ShardCoordinatorCore& core() const {
        return core_;
    }

    void register_actor(const LogicalActorId& id, const std::string& owner_node) {
        core_.register_actor(id, owner_node);
    }

    void unregister_actor(const LogicalActorId& id) {
        core_.unregister_actor(id);
    }

    std::string get_shard_owner(ShardId shard) const {
        return core_.get_shard_owner(shard);
    }

    void rebalance(const std::vector<std::string>& alive_nodes) {
        core_.rebalance(alive_nodes);
    }

  private:
    ShardCoordinatorCore core_;
};

} // namespace hpactor::cluster::sharding
