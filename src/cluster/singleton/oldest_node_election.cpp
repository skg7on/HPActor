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

#include <hpactor/cluster/singleton/oldest_node_election.hpp>

#include <algorithm>

namespace hpactor::cluster::singleton {

std::optional<std::string>
OldestNodeElection::elect(const SingletonIdentity& /*id*/,
                          std::span<const std::string> alive_nodes) {
    if (alive_nodes.empty())
        return std::nullopt;

    std::string best = std::string(alive_nodes[0]);
    for (size_t i = 1; i < alive_nodes.size(); ++i) {
        if (alive_nodes[i] < best) {
            best = std::string(alive_nodes[i]);
        }
    }
    return best;
}

void OldestNodeElection::on_peer_down(const std::string& node_id) {
    known_dead_.insert(node_id);
}

} // namespace hpactor::cluster::singleton
