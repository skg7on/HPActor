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

/// \brief Majority-vote singleton election strategy.
///
/// A node becomes leader when it receives votes from a strict majority
/// (> N/2) of the currently alive nodes. Votes are accumulated via
/// gossip piggyback and fed into the election via record_vote().
///
/// This provides stronger safety than oldest-node-wins: a leader cannot
/// be elected during a partition where < majority of the cluster agrees.
///
/// \note Vote counting is per-singleton-name, so multiple singletons can
///       have independent election outcomes.
class MajorityBasedElection : public ISingletonElection {
  public:
    /// \brief Record a vote from a node for a specific singleton.
    ///
    /// Called by SingletonManagerActor when a gossip piggyback carries
    /// a vote entry. Overwrites any previous vote from the same voter
    /// for the same singleton.
    ///
    /// \param[in] singleton_name The singleton being voted on.
    /// \param[in] voter The node casting the vote.
    /// \param[in] voted_for The node the voter supports as owner.
    void record_vote(const std::string& singleton_name,
                     const std::string& voter, const std::string& voted_for);

    std::optional<std::string>
    elect(const SingletonIdentity& id,
          std::span<const std::string> alive_nodes) override;

    void on_peer_down(const std::string& node_id) override;

  private:
    /// Per-singleton: voter_id → voted_for_node_id
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> votes_;

    /// Known-dead nodes whose votes should be excluded from counts.
    std::unordered_set<std::string> known_dead_;
};

} // namespace hpactor::cluster::singleton
