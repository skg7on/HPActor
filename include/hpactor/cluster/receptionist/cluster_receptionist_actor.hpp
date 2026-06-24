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

#include <hpactor/cluster/receptionist/cluster_receptionist_core.hpp>

#include <string>
#include <vector>

namespace hpactor::cluster::receptionist {

/// \brief Actor wrapper around ClusterReceptionistCore.
///
/// Thin delegate-to-core wrapper following the Core/Actor separation
/// pattern. Manages cluster-wide ServiceKey registration and discovery.
///
/// Future evolution: When cluster mode is fully wired, this becomes an
/// EventBasedActor that subscribes to local Receptionist changes and
/// gossips them cluster-wide.
///
/// \note Delegates to a thread-safe core; callers must still synchronize
///       access when sharing the wrapper across threads.
class ClusterReceptionistActor {
  public:
    /// \brief Default-construct with an empty core.
    ClusterReceptionistActor() : core_() {}

    /// \brief Access the underlying core (mutable).
    ///
    /// \return Reference to the ClusterReceptionistCore.
    ClusterReceptionistCore& core() {
        return core_;
    }

    /// \brief Access the underlying core (const).
    ///
    /// \return Const reference to the ClusterReceptionistCore.
    const ClusterReceptionistCore& core() const {
        return core_;
    }

    // --- Delegates ---

    /// \brief Apply local registration under a ServiceKey (delegates to core).
    ///
    /// \param[in] key The ServiceKey to register under.
    /// \param[in] actor_ids The local actor IDs to register.
    void apply_local_registration(const hpactor::receptionist::ServiceKey& key,
                                  const std::vector<uint64_t>& actor_ids) {
        core_.apply_local_registration(key, actor_ids);
    }

    /// \brief Merge a remote registration from gossip (delegates to core).
    ///
    /// \param[in] reg The remote registration to merge.
    void merge_remote_registration(const ClusterRegistration& reg) {
        core_.merge_remote_registration(reg);
    }

    /// \brief Remove all registrations from a node (delegates to core).
    ///
    /// \param[in] node_id The node whose registrations are purged.
    void remove_node_registrations(const std::string& node_id) {
        core_.remove_node_registrations(node_id);
    }

    /// \brief Get cluster-wide listing for a key (delegates to core).
    ///
    /// \param[in] key The ServiceKey to query.
    /// \return Combined vector of local and remote actor IDs.
    std::vector<uint64_t>
    get_cluster_listing(const hpactor::receptionist::ServiceKey& key) const {
        return core_.get_cluster_listing(key);
    }

    /// \brief Check if a key exists (delegates to core).
    ///
    /// \param[in] key The ServiceKey to check.
    /// \return \c true if the key has local or remote registrations.
    bool has_key(const hpactor::receptionist::ServiceKey& key) const {
        return core_.has_key(key);
    }

    /// \brief Get remote key count (delegates to core).
    ///
    /// \return Number of keys with remote registrations.
    size_t remote_key_count() const {
        return core_.remote_key_count();
    }

    /// \brief Get local key count (delegates to core).
    ///
    /// \return Number of locally registered keys.
    size_t local_key_count() const {
        return core_.local_key_count();
    }

    /// \brief Drain dirty registrations for gossip (delegates to core).
    ///
    /// \return Vector of dirty ClusterRegistration entries.
    std::vector<ClusterRegistration> drain_dirty_registrations() {
        return core_.drain_dirty_registrations();
    }

  private:
    ClusterReceptionistCore core_;
};

} // namespace hpactor::cluster::receptionist
