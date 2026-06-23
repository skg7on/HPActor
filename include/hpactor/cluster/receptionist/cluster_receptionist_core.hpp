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

#include <hpactor/cluster/receptionist/cluster_receptionist_messages.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::cluster::receptionist {

/// \brief Thread-safe core for cluster-wide ServiceKey registration.
///
/// Manages cross-node ServiceKey registrations. Each node gossips its
/// local registrations; other nodes merge them to build a cluster-wide
/// view. When a node goes Down, its registrations are purged.
///
/// \note All methods are thread-safe (protected by internal mutex).
class ClusterReceptionistCore {
  public:
    /// \brief Apply local registration under a ServiceKey.
    void apply_local_registration(const hpactor::receptionist::ServiceKey& key,
                                  const std::vector<uint64_t>& actor_ids);

    /// \brief Merge a remote registration entry from gossip.
    void merge_remote_registration(const ClusterRegistration& reg);

    /// \brief Remove all registrations from a node (node-down cleanup).
    void remove_node_registrations(const std::string& node_id);

    /// \brief Get all actor IDs registered under a key cluster-wide.
    std::vector<uint64_t>
    get_cluster_listing(const hpactor::receptionist::ServiceKey& key) const;

    /// \brief Check if a key has any registrations (local or remote).
    bool has_key(const hpactor::receptionist::ServiceKey& key) const;

    /// \brief Number of keys with remote registrations.
    size_t remote_key_count() const;

    /// \brief Number of local keys registered.
    size_t local_key_count() const;

    /// \brief Get local registrations that need gossip dissemination.
    std::vector<ClusterRegistration> drain_dirty_registrations();

  private:
    mutable std::mutex mutex_;

    // Local: key → set of actor_ids
    std::unordered_map<hpactor::receptionist::ServiceKey, std::vector<uint64_t>> local_regs_;

    // Remote: key → node_id → registration
    std::unordered_map<hpactor::receptionist::ServiceKey,
                       std::unordered_map<std::string, ClusterRegistration>>
        remote_regs_;

    // Dirty local registrations needing gossip
    std::vector<ClusterRegistration> dirty_regs_;
};

} // namespace hpactor::cluster::receptionist
