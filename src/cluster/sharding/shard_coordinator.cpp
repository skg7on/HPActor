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

#include <hpactor/cluster/sharding/shard_coordinator.hpp>
#include <hpactor/cluster/sharding/shard_resolver.hpp>

namespace hpactor::cluster::sharding {

ShardCoordinatorCore::ShardCoordinatorCore(uint32_t total_shards,
                                           std::unique_ptr<IPlacementStrategy> strategy)
    : total_shards_(total_shards), strategy_(std::move(strategy)) {}

uint32_t ShardCoordinatorCore::total_shards() const {
    return total_shards_;
}

void ShardCoordinatorCore::register_actor(const LogicalActorId& id,
                                          const std::string& owner_node) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = id.persistence_id;
    if (registered_actors_.find(key) != registered_actors_.end()) {
        return; // Already registered — idempotent
    }

    registered_actors_.insert(key);

    ShardId shard = ShardResolver::resolve(id, total_shards_);
    uint64_t new_epoch = shard_table_.epoch() + 1;
    shard_table_.update(shard, owner_node, new_epoch);
}

void ShardCoordinatorCore::unregister_actor(const LogicalActorId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    registered_actors_.erase(id.persistence_id);
}

bool ShardCoordinatorCore::is_actor_registered(const LogicalActorId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registered_actors_.find(id.persistence_id) != registered_actors_.end();
}

std::string ShardCoordinatorCore::get_shard_owner(ShardId shard) const {
    auto entry = shard_table_.lookup(shard);
    if (entry.has_value())
        return entry->owner_node;
    return {};
}

void ShardCoordinatorCore::rebalance(const std::vector<std::string>& alive_nodes) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (alive_nodes.empty()) {
        shard_table_.clear();
        return;
    }

    // Build list of all shard IDs
    std::vector<ShardId> all_shards(total_shards_);
    for (uint32_t i = 0; i < total_shards_; ++i) {
        all_shards[i] = i;
    }

    // Collect current assignments for movement-minimizing rebalancing
    std::vector<ShardEntry> current = shard_table_.entries();

    auto plan = strategy_->plan(all_shards, alive_nodes, current);

    uint64_t new_epoch = shard_table_.epoch() + 1;
    for (const auto& [shard_id, owner] : plan.assignments) {
        shard_table_.update(shard_id, owner, new_epoch);
    }
}

uint64_t ShardCoordinatorCore::epoch() const {
    return shard_table_.epoch();
}

const ShardTable& ShardCoordinatorCore::shard_table() const {
    return shard_table_;
}

// ── Leadership token validation ───────────────────────────────────────────

void ShardCoordinatorCore::set_active_lease(const singleton::LeadershipLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_lease_ = lease;
}

void ShardCoordinatorCore::clear_active_lease() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_lease_.reset();
}

bool ShardCoordinatorCore::validate_token(uint64_t fencing_token,
                                          std::string_view singleton_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_lease_.has_value())
        return false;
    if (active_lease_->singleton_name != singleton_name)
        return false;
    // Accept if the incoming token matches, or if the incoming lease's
    // fences() subsumes the active lease token (future CLU-003 extension).
    return fencing_token == active_lease_->fencing_token;
}

} // namespace hpactor::cluster::sharding
