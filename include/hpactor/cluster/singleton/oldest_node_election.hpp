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
#include <unordered_set>

namespace hpactor::cluster::singleton {

/// \brief Oldest-node-wins election: lowest NodeId (lexicographic) wins.
///
/// Simple, deterministic, no consensus needed. Sufficient for singleton
/// failover in phase 1.
class OldestNodeElection : public ISingletonElection {
  public:
    std::optional<std::string>
    elect(const SingletonIdentity& id,
          std::span<const std::string> alive_nodes) override;

    void on_peer_down(const std::string& node_id) override;

  private:
    std::unordered_set<std::string> known_dead_;
};

} // namespace hpactor::cluster::singleton
