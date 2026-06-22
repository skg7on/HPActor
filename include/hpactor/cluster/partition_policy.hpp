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

namespace hpactor::cluster {

/// \brief Partition behavior policy.
enum class PartitionPolicy : uint8_t {
    FailOpen,       ///< Continue best-effort delivery to reachable nodes.
    FailClosed,     ///< Stop remote delivery unless quorum is known.
    StaticMajority, ///< Only configured majority partition owns coordinator
                    ///< roles.
};

/// \brief Whether user messages may be delivered under this policy.
constexpr bool
allow_user_delivery(PartitionPolicy policy, bool quorum_known) noexcept {
    switch (policy) {
        case PartitionPolicy::FailOpen:
            return true;
        case PartitionPolicy::FailClosed:
            return quorum_known;
        case PartitionPolicy::StaticMajority:
            return quorum_known;
    }
    return false;
}

/// \brief Whether ownership changes are permitted under this policy.
constexpr bool
allow_ownership_change(PartitionPolicy policy, bool majority_present) noexcept {
    switch (policy) {
        case PartitionPolicy::FailOpen:
        case PartitionPolicy::FailClosed:
        case PartitionPolicy::StaticMajority:
            return majority_present;
    }
    return false;
}

/// \brief Human-readable snake_case string for the policy.
constexpr const char* to_string(PartitionPolicy policy) noexcept {
    switch (policy) {
        case PartitionPolicy::FailOpen:
            return "fail_open";
        case PartitionPolicy::FailClosed:
            return "fail_closed";
        case PartitionPolicy::StaticMajority:
            return "static_majority";
    }
    return "unknown";
}

} // namespace hpactor::cluster
