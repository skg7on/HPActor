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

#include <cstdint>
#include <string>

namespace hpactor::cluster::singleton {

/// \brief Identity for a cluster singleton.
struct SingletonIdentity {
    std::string name;           ///< Logical name (e.g., "shard-coordinator").
    uint64_t fencing_token = 0; ///< Increments on each ownership change.
};

/// \brief Lifecycle state of a singleton on this node.
enum class SingletonState : uint8_t {
    Standby,    ///< Not the owner; monitoring the active owner.
    Activating, ///< Becoming owner (transitional).
    Active,     ///< Current owner; processing messages.
    Draining,   ///< Graceful handoff in progress.
};

/// \brief Human-readable snake_case string for the singleton state.
constexpr const char* to_string(SingletonState state) noexcept {
    switch (state) {
        case SingletonState::Standby:
            return "standby";
        case SingletonState::Activating:
            return "activating";
        case SingletonState::Active:
            return "active";
        case SingletonState::Draining:
            return "draining";
    }
    return "unknown";
}

} // namespace hpactor::cluster::singleton
