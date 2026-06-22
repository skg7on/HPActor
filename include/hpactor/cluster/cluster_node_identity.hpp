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

#include <cstdint>
#include <string>

namespace hpactor::cluster {

/// \brief Logical cluster namespace.
using ClusterId = std::string;

/// \brief Node identity fields carried on every transport connection and
///        membership advertisement.
struct ClusterNodeIdentity {
    std::string node_id;      ///< Stable logical node name.
    uint64_t incarnation = 0; ///< Monotonic per-start counter; higher wins.
    uint64_t process_start_id = 0; ///< Boot-id or monotonic counter per process
                                   ///< start.
    uint64_t membership_epoch = 0; ///< Cluster membership generation.
    ClusterId cluster_id;          ///< Cluster namespace.
};

/// \brief Whether \p newer fences \p older — same node, higher incarnation.
constexpr bool fences(const ClusterNodeIdentity& newer,
                      const ClusterNodeIdentity& older) noexcept {
    return newer.node_id == older.node_id && newer.incarnation > older.incarnation;
}

/// \brief Whether two identities represent a conflict — same node, same
///        incarnation, different process start IDs.
constexpr bool is_identity_conflict(const ClusterNodeIdentity& a,
                                    const ClusterNodeIdentity& b) noexcept {
    return a.node_id == b.node_id && a.incarnation == b.incarnation &&
           a.process_start_id != b.process_start_id;
}

/// \brief Whether two identities belong to the same cluster namespace.
inline bool
same_cluster(const ClusterNodeIdentity& a, const ClusterNodeIdentity& b) {
    return a.cluster_id == b.cluster_id;
}

/// \brief Whether \p candidate has a membership epoch older than \p current.
constexpr bool has_stale_epoch(const ClusterNodeIdentity& candidate,
                               const ClusterNodeIdentity& current) noexcept {
    return candidate.membership_epoch < current.membership_epoch;
}

} // namespace hpactor::cluster
