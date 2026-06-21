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
#include <hpactor/cluster/singleton/singleton_identity.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::cluster::singleton {

/// \brief Core singleton ownership logic for one node.
///
/// Manages singleton registration, election-based activation, and
/// fencing token management. Thread-safe. Tested directly.
///
/// In CLU-003 integration, wrapped by an EventBasedActor for
/// cluster event handling.
class SingletonManagerCore {
  public:
    /// \param[in] self_node This node's ID.
    /// \param[in] election Election strategy (takes ownership).
    SingletonManagerCore(std::string self_node,
                         std::unique_ptr<ISingletonElection> election);

    const std::string& self_node() const;

    /// \brief Register a singleton to be managed.
    void register_singleton(const SingletonIdentity& id);

    /// \brief Re-run elections for all registered singletons.
    void on_node_state_change(const std::vector<std::string>& alive_nodes);

    /// \brief Get the current state of a singleton on this node.
    SingletonState get_state(const std::string& name) const;

    /// \brief Get the current fencing token for a singleton.
    uint64_t get_fencing_token(const std::string& name) const;

    /// \brief Begin draining a singleton (Active → Draining).
    bool begin_drain(const std::string& name);

    /// \brief Complete draining (Draining → Standby).
    bool complete_drain(const std::string& name);

    /// \brief Number of registered singletons.
    size_t singleton_count() const;

  private:
    struct SingletonRecord {
        SingletonIdentity identity;
        SingletonState state = SingletonState::Standby;
    };

    std::string self_node_;
    std::unique_ptr<ISingletonElection> election_;
    std::unordered_map<std::string, SingletonRecord> singletons_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::cluster::singleton
