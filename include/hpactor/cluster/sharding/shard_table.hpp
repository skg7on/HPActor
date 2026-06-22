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

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::cluster::sharding {

/// \brief Local cached shard ownership table.
///
/// Thread-safe. Updated by ShardCoordinator epoch publications and
/// ShardMoved control frames. Invalidated on node-down events.
class ShardTable {
  public:
    ShardTable() = default;

    /// \brief Look up the current owner of a shard.
    std::optional<ShardEntry> lookup(ShardId shard) const;

    /// \brief Update or insert a shard entry. Ignores stale epochs.
    void update(ShardId shard, const std::string& owner_node, uint64_t epoch);

    /// \brief Remove all entries for a specific node (called on node-down).
    void invalidate_for_node(const std::string& node_id);

    /// \brief Remove all entries.
    void clear();

    /// \brief Return all entries as a snapshot.
    std::vector<ShardEntry> entries() const;

    /// \brief Current highest epoch seen.
    uint64_t epoch() const;

  private:
    std::unordered_map<ShardId, ShardEntry> entries_;
    uint64_t current_epoch_ = 0;
    mutable std::mutex mutex_;
};

} // namespace hpactor::cluster::sharding
