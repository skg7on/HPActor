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
class ClusterReceptionistActor {
  public:
    ClusterReceptionistActor() : core_() {}

    ClusterReceptionistCore& core() {
        return core_;
    }
    const ClusterReceptionistCore& core() const {
        return core_;
    }

    // --- Delegates ---

    void apply_local_registration(const hpactor::receptionist::ServiceKey& key,
                                  const std::vector<uint64_t>& actor_ids) {
        core_.apply_local_registration(key, actor_ids);
    }

    void merge_remote_registration(const ClusterRegistration& reg) {
        core_.merge_remote_registration(reg);
    }

    void remove_node_registrations(const std::string& node_id) {
        core_.remove_node_registrations(node_id);
    }

    std::vector<uint64_t>
    get_cluster_listing(const hpactor::receptionist::ServiceKey& key) const {
        return core_.get_cluster_listing(key);
    }

    bool has_key(const hpactor::receptionist::ServiceKey& key) const {
        return core_.has_key(key);
    }

    size_t remote_key_count() const {
        return core_.remote_key_count();
    }
    size_t local_key_count() const {
        return core_.local_key_count();
    }

    std::vector<ClusterRegistration> drain_dirty_registrations() {
        return core_.drain_dirty_registrations();
    }

  private:
    ClusterReceptionistCore core_;
};

} // namespace hpactor::cluster::receptionist
