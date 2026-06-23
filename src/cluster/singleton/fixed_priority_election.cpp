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

#include <hpactor/cluster/singleton/fixed_priority_election.hpp>

#include <algorithm>
#include <limits>

namespace hpactor::cluster::singleton {

FixedPriorityElection::FixedPriorityElection(std::unordered_map<std::string, int> priorities)
    : priorities_(std::move(priorities)) {}

std::optional<std::string>
FixedPriorityElection::elect(const SingletonIdentity& /*id*/,
                             std::span<const std::string> alive_nodes) {
    if (alive_nodes.empty())
        return std::nullopt;

    int best_priority = std::numeric_limits<int>::min();
    std::optional<std::string> best_node;

    for (const auto& node : alive_nodes) {
        std::string node_str(node);
        auto it = priorities_.find(node_str);
        if (it == priorities_.end())
            continue; // Node has no priority — skip

        int priority = it->second;
        if (priority > best_priority ||
            (priority == best_priority && best_node.has_value() &&
             node_str < *best_node)) {
            best_priority = priority;
            best_node = node_str;
        }
    }

    return best_node;
}

void FixedPriorityElection::on_peer_down(const std::string& node_id) {
    known_dead_.insert(node_id);
}

} // namespace hpactor::cluster::singleton
