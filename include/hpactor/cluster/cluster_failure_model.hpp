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

#include <hpactor/cluster/cluster_node_identity.hpp>
#include <hpactor/cluster/cluster_node_state.hpp>
#include <hpactor/cluster/partition_policy.hpp>

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::cluster {

/// \brief Result of a state transition request.
struct TransitionResult {
    bool success = false;
    bool routes_invalidated = false;
    std::string reason;
};

/// \brief Resolution for an identity conflict.
enum class IdentityConflictResolution : uint8_t {
    None,
    QuarantineBoth,
    FenceOld,
};

/// \brief Result of an identity conflict check.
struct IdentityCheckStatus {
    bool conflict_detected = false;
    IdentityConflictResolution resolution = IdentityConflictResolution::None;
    bool fence_old = false;
};

/// \brief Manages cluster node state, fencing, and route invalidation.
///
/// Owns the authoritative node state map. Receives membership events from
/// GossipMembership and applies policy decisions. Coordinates route
/// invalidation when nodes enter Down, Quarantined, or Removed states.
///
/// \note Thread safety: All public methods are safe to call from any thread.
class ClusterFailureModel {
  public:
    ClusterFailureModel();
    ~ClusterFailureModel();

    bool register_node(const ClusterNodeIdentity& id);
    TransitionResult transition(const std::string& node_id, ClusterNodeState to,
                                const std::string& reason);
    ClusterNodeState get_state(const std::string& node_id) const;
    size_t node_count() const;

    IdentityCheckStatus
    check_identity_conflict(const ClusterNodeIdentity& received) const;

    PartitionPolicy get_partition_policy() const;
    void set_partition_policy(PartitionPolicy policy);

    bool quorum_present() const;
    std::vector<std::string> alive_nodes() const;
    std::vector<std::string> drain_invalidation_queue();

    /// \brief Observer callback type for node state change notifications.
    using StateChangeObserver =
        std::function<void(const std::vector<std::string>& alive_nodes)>;

    /// \brief Register an observer for node state change notifications.
    ///
    /// Observers are called after every successful transition AFTER the
    /// internal mutex has been released. Observers may safely acquire
    /// other locks without risk of deadlock with the cluster failure model.
    void register_observer(StateChangeObserver observer);

  private:
    struct NodeRecord {
        ClusterNodeIdentity identity;
        ClusterNodeState state = ClusterNodeState::Joining;
    };

    std::unordered_map<std::string, NodeRecord> nodes_;
    PartitionPolicy partition_policy_ = PartitionPolicy::FailOpen;
    std::vector<std::string> invalidation_queue_;
    std::vector<StateChangeObserver> observers_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::cluster
