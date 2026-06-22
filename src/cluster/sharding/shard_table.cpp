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

#include <hpactor/cluster/sharding/shard_table.hpp>

namespace hpactor::cluster::sharding {

std::optional<ShardEntry> ShardTable::lookup(ShardId shard) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(shard);
    if (it == entries_.end())
        return std::nullopt;
    return it->second;
}

void ShardTable::update(ShardId shard, const std::string& owner_node,
                        uint64_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(shard);
    if (it != entries_.end() && it->second.epoch >= epoch) {
        return; // ignore stale or same epoch
    }
    entries_[shard] = {shard, owner_node, epoch};
    if (epoch > current_epoch_)
        current_epoch_ = epoch;
}

void ShardTable::invalidate_for_node(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.owner_node == node_id) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void ShardTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    current_epoch_ = 0;
}

uint64_t ShardTable::epoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_epoch_;
}

} // namespace hpactor::cluster::sharding
