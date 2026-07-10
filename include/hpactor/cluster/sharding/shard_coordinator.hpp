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

#include <hpactor/cluster/sharding/placement_strategy.hpp>
#include <hpactor/cluster/sharding/shard_table.hpp>
#include <hpactor/cluster/sharding/shard_types.hpp>
#include <hpactor/cluster/singleton/leadership_lease.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace hpactor::cluster::sharding {

/// \brief Core coordinator logic for shard ownership management.
///
/// Owns the authoritative shard table and placement strategy. Manages
/// actor registration and rebalancing. Thread-safe.
///
/// In CLU-003, this will be wrapped in an EventBasedActor singleton.
/// For now, it is tested directly via its public API.
class ShardCoordinatorCore {
  public:
    /// \param[in] total_shards Total number of shards in the system.
    /// \param[in] strategy Placement strategy (takes ownership).
    ShardCoordinatorCore(uint32_t total_shards,
                         std::unique_ptr<IPlacementStrategy> strategy);

    uint32_t total_shards() const;

    /// \brief Register a logical actor on a node. Assigns its shard.
    void register_actor(const LogicalActorId& id, const std::string& owner_node);

    /// \brief Unregister a logical actor.
    void unregister_actor(const LogicalActorId& id);

    /// \brief Check if a logical actor is registered.
    bool is_actor_registered(const LogicalActorId& id) const;

    /// \brief Get the current owner of a shard.
    std::string get_shard_owner(ShardId shard) const;

    /// \brief Rebalance all shards across the given alive nodes.
    void rebalance(const std::vector<std::string>& alive_nodes);

    /// \brief Current shard table epoch.
    uint64_t epoch() const;

    /// \brief Access the shard table for inspection.
    const ShardTable& shard_table() const;

    // ── Leadership token validation (CLU-003 integration) ──────────

    /// \brief Set the active leadership lease for token validation.
    void set_active_lease(const singleton::LeadershipLease& lease);

    /// \brief Clear the active lease, rejecting all subsequent tokens.
    void clear_active_lease();

    /// \brief Validate a fencing token against the active lease.
    ///
    /// \return true if the token matches the active lease for the given
    ///         singleton name.
    [[nodiscard]] bool
    validate_token(uint64_t fencing_token, std::string_view singleton_name) const;

  private:
    uint32_t total_shards_;
    std::unique_ptr<IPlacementStrategy> strategy_;
    ShardTable shard_table_;
    std::unordered_set<std::string> registered_actors_;
    std::optional<singleton::LeadershipLease> active_lease_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::cluster::sharding
