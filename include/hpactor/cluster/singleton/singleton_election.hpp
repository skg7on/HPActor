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

#include <hpactor/cluster/singleton/singleton_identity.hpp>

#include <optional>
#include <span>
#include <string>

namespace hpactor::cluster::singleton {

/// \brief Interface for singleton leader election strategies.
class ISingletonElection {
  public:
    virtual ~ISingletonElection() = default;

    /// \brief Elect the owner for a singleton from alive nodes.
    ///
    /// \param[in] id The singleton identity.
    /// \param[in] alive_nodes Currently alive node IDs.
    /// \return The elected owner, or nullopt if no election possible.
    virtual std::optional<std::string>
    elect(const SingletonIdentity& id, std::span<const std::string> alive_nodes) = 0;

    /// \brief Notify the election strategy that a peer is down.
    virtual void on_peer_down(const std::string& node_id) = 0;
};

} // namespace hpactor::cluster::singleton
