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
#include <hpactor/cluster/singleton/singleton_manager.hpp>

#include <memory>
#include <string>
#include <vector>

namespace hpactor::cluster::singleton {

/// \brief Actor wrapper around SingletonManagerCore.
///
/// Provides actor-friendly API for registration, node state change
/// handling, and drain lifecycle. Inline methods delegate to core.
class SingletonManagerActor {
  public:
    SingletonManagerActor(std::string self_node,
                          std::unique_ptr<ISingletonElection> election)
        : core_(std::move(self_node), std::move(election)) {}

    const std::string& self_node() const {
        return core_.self_node();
    }
    SingletonManagerCore& core() {
        return core_;
    }
    const SingletonManagerCore& core() const {
        return core_;
    }

    void register_singleton(const SingletonIdentity& id) {
        core_.register_singleton(id);
    }

    void on_node_state_change(const std::vector<std::string>& alive_nodes) {
        core_.on_node_state_change(alive_nodes);
    }

    bool begin_drain(const std::string& name) {
        return core_.begin_drain(name);
    }

    bool complete_drain(const std::string& name) {
        return core_.complete_drain(name);
    }

  private:
    SingletonManagerCore core_;
};

} // namespace hpactor::cluster::singleton
