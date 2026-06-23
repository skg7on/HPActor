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

#include <hpactor/actor/receptionist/receptionist_messages.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::cluster::receptionist {

/// \brief A cross-node registration entry for a ServiceKey.
struct ClusterRegistration {
    hpactor::receptionist::ServiceKey key;
    std::string node_id;
    std::vector<uint64_t> actor_ids; ///< ActorIds registered under this key.
    uint64_t incarnation = 0;        ///< For conflict resolution.

    bool operator==(const ClusterRegistration& other) const {
        return key == other.key && node_id == other.node_id &&
               incarnation == other.incarnation;
    }
};

} // namespace hpactor::cluster::receptionist
