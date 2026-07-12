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
#include <hpactor/cluster/singleton/leadership_lease.hpp>

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

    // DEPRECATED: Use rebalance_with_token() for production.
    // This method is kept for tests and internal use only.
    void rebalance(const std::vector<std::string>& alive_nodes) {
        core_.rebalance(alive_nodes);
    }

    // ── Leadership lease management (CLU-003 integration) ──────────

    /// \brief Update the active leadership lease for token-gated operations.
    void on_lease_update(const singleton::LeadershipLease& lease) {
        core_.set_active_lease(lease);
    }

    /// \brief Rebalance shards with token validation.
    ///
    /// \return true if token is valid and rebalance was executed.
    bool rebalance_with_token(const std::vector<std::string>& alive_nodes,
                              uint64_t fencing_token) {
        if (!core_.validate_token(fencing_token, "shard-coordinator")) {
            return false;
        }
        core_.rebalance(alive_nodes);
        return true;
    }

  private:
    ShardCoordinatorCore core_;
};

} // namespace hpactor::cluster::sharding
