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

#include <hpactor/cluster/receptionist/cluster_receptionist_core.hpp>

namespace hpactor::cluster::receptionist {

void ClusterReceptionistCore::apply_local_registration(
    const hpactor::receptionist::ServiceKey& key,
    const std::vector<uint64_t>& actor_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    local_regs_[key] = actor_ids;

    ClusterRegistration dirty;
    dirty.key = key;
    dirty.actor_ids = actor_ids;
    dirty.incarnation = 0;
    dirty_regs_.push_back(dirty);
}

void ClusterReceptionistCore::merge_remote_registration(const ClusterRegistration& reg) {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_regs_[reg.key][reg.node_id] = reg;
}

void ClusterReceptionistCore::remove_node_registrations(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, node_map] : remote_regs_) {
        node_map.erase(node_id);
    }
    // Also clean up keys with no remaining registrations
    for (auto it = remote_regs_.begin(); it != remote_regs_.end();) {
        if (it->second.empty()) {
            it = remote_regs_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<uint64_t> ClusterReceptionistCore::get_cluster_listing(
    const hpactor::receptionist::ServiceKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<uint64_t> result;

    // Local first
    auto lit = local_regs_.find(key);
    if (lit != local_regs_.end()) {
        result.insert(result.end(), lit->second.begin(), lit->second.end());
    }

    // Then remote
    auto rit = remote_regs_.find(key);
    if (rit != remote_regs_.end()) {
        for (const auto& [node_id, reg] : rit->second) {
            result.insert(result.end(), reg.actor_ids.begin(), reg.actor_ids.end());
        }
    }

    return result;
}

bool ClusterReceptionistCore::has_key(const hpactor::receptionist::ServiceKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_regs_.find(key) != local_regs_.end() ||
           remote_regs_.find(key) != remote_regs_.end();
}

size_t ClusterReceptionistCore::remote_key_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return remote_regs_.size();
}

size_t ClusterReceptionistCore::local_key_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_regs_.size();
}

std::vector<ClusterRegistration>
ClusterReceptionistCore::drain_dirty_registrations() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ClusterRegistration> result;
    result.swap(dirty_regs_);
    return result;
}

} // namespace hpactor::cluster::receptionist
