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

#include <hpactor/cluster/cluster_failure_model.hpp>

#include <algorithm>
#include <mutex>

namespace hpactor::cluster {

ClusterFailureModel::ClusterFailureModel() = default;
ClusterFailureModel::~ClusterFailureModel() = default;

bool ClusterFailureModel::register_node(const ClusterNodeIdentity& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodes_.find(id.node_id) != nodes_.end()) {
        return false;
    }
    nodes_[id.node_id] = NodeRecord{id, ClusterNodeState::Joining};
    return true;
}

TransitionResult
ClusterFailureModel::transition(const std::string& node_id, ClusterNodeState to,
                                const std::string& /*reason*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return {false, false, "node not found"};
    }

    ClusterNodeState from = it->second.state;

    if (!can_transition(from, to)) {
        return {false, false, "illegal transition"};
    }

    it->second.state = to;

    bool invalidated =
        (to == ClusterNodeState::Down || to == ClusterNodeState::Quarantined ||
         to == ClusterNodeState::Removed);

    if (invalidated) {
        invalidation_queue_.push_back(node_id);
    }

    return {true, invalidated, ""};
}

ClusterNodeState ClusterFailureModel::get_state(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return ClusterNodeState::Removed;
    }
    return it->second.state;
}

size_t ClusterFailureModel::node_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_.size();
}

IdentityCheckStatus
ClusterFailureModel::check_identity_conflict(const ClusterNodeIdentity& received) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = nodes_.find(received.node_id);
    if (it == nodes_.end()) {
        return {};
    }

    const auto& existing = it->second.identity;

    if (is_identity_conflict(received, existing)) {
        return {true, IdentityConflictResolution::QuarantineBoth, false};
    }

    if (fences(received, existing)) {
        return {false, IdentityConflictResolution::FenceOld, true};
    }

    return {};
}

PartitionPolicy ClusterFailureModel::get_partition_policy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return partition_policy_;
}

void ClusterFailureModel::set_partition_policy(PartitionPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    partition_policy_ = policy;
}

bool ClusterFailureModel::quorum_present() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodes_.empty())
        return false;

    size_t alive_count = 0;
    for (const auto& pair : nodes_) {
        if (pair.second.state == ClusterNodeState::Alive) {
            ++alive_count;
        }
    }
    return alive_count > (nodes_.size() / 2);
}

std::vector<std::string> ClusterFailureModel::alive_nodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& pair : nodes_) {
        if (pair.second.state == ClusterNodeState::Alive) {
            result.push_back(pair.first);
        }
    }
    return result;
}

std::vector<std::string> ClusterFailureModel::drain_invalidation_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> drained;
    drained.swap(invalidation_queue_);
    return drained;
}

} // namespace hpactor::cluster
