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

#include <hpactor/cluster/singleton/singleton_election.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hpactor::cluster::singleton {

/// \brief Fixed-priority singleton election strategy.
///
/// Each node is assigned a static integer priority at construction time.
/// The highest-priority node among the alive set is elected. Ties are
/// broken by lowest node_id (lexicographic).
///
/// Useful for controlled-failover scenarios where a specific node should
/// always be the preferred singleton owner (e.g., a primary data center).
///
/// \note Nodes without a priority entry are excluded from election.
class FixedPriorityElection : public ISingletonElection {
  public:
    /// \brief Construct with a priority map.
    ///
    /// \param[in] priorities Map of node_id → priority (higher = preferred).
    explicit FixedPriorityElection(std::unordered_map<std::string, int> priorities);

    /// \brief Elect the owner for the singleton from the alive node set.
    ///
    /// Selects the alive node with the highest priority. Ties are broken
    /// by lexicographically lowest \c node_id. Nodes without a priority
    /// entry are skipped.
    ///
    /// \param[in] id The singleton identity (unused by this strategy).
    /// \param[in] alive_nodes Currently alive node IDs.
    /// \return The elected owner node ID, or \c std::nullopt if no
    ///         alive node has a priority entry.
    std::optional<std::string>
    elect(const SingletonIdentity& id,
          std::span<const std::string> alive_nodes) override;

    /// \brief Notify the election strategy that a peer node is down.
    ///
    /// Records the node in the known-dead set for future introspection.
    ///
    /// \param[in] node_id The node that went down.
    void on_peer_down(const std::string& node_id) override;

  private:
    std::unordered_map<std::string, int> priorities_;
    std::unordered_set<std::string> known_dead_;
};

} // namespace hpactor::cluster::singleton
