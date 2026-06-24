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

#include <hpactor/cluster/singleton/majority_based_election.hpp>

#include <algorithm>

namespace hpactor::cluster::singleton {

void MajorityBasedElection::record_vote(const std::string& singleton_name,
                                        const std::string& voter,
                                        const std::string& voted_for) {
    votes_[singleton_name][voter] = voted_for;
}

std::optional<std::string>
MajorityBasedElection::elect(const SingletonIdentity& id,
                             std::span<const std::string> alive_nodes) {
    if (alive_nodes.empty())
        return std::nullopt;

    const size_t majority_threshold = (alive_nodes.size() / 2) + 1;

    auto it = votes_.find(id.name);
    if (it == votes_.end())
        return std::nullopt;

    const auto& singleton_votes = it->second;

    // Pre-build alive set for O(1) candidate liveness check
    std::unordered_set<std::string> alive_set(alive_nodes.begin(),
                                              alive_nodes.end());

    // Count live votes per candidate (exclude dead voters)
    std::unordered_map<std::string, size_t> tally;
    for (const auto& alive : alive_nodes) {
        auto vit = singleton_votes.find(std::string(alive));
        if (vit != singleton_votes.end()) {
            const auto& candidate = vit->second;
            // Only count if the candidate is still alive
            if (alive_set.find(candidate) == alive_set.end())
                continue;
            tally[candidate]++;
        }
    }

    // Find candidate with ≥ majority_threshold votes
    for (const auto& [candidate, count] : tally) {
        if (count >= majority_threshold)
            return candidate;
    }

    return std::nullopt;
}

void MajorityBasedElection::on_peer_down(const std::string& node_id) {
    known_dead_.insert(node_id);
    // Remove votes cast by the dead node from all singletons
    for (auto& [name, singleton_votes] : votes_) {
        singleton_votes.erase(node_id);
    }
}

} // namespace hpactor::cluster::singleton
