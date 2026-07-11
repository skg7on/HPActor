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

#include <hpactor/cluster/singleton/leadership_status.hpp>

namespace hpactor::cluster::singleton {

const char* to_string(LeadershipStatusCode code) noexcept {
    switch (code) {
        case LeadershipStatusCode::Granted:
            return "granted";
        case LeadershipStatusCode::AlreadyOwned:
            return "already_owned";
        case LeadershipStatusCode::Renewed:
            return "renewed";
        case LeadershipStatusCode::Released:
            return "released";
        case LeadershipStatusCode::Lost:
            return "lost";
        case LeadershipStatusCode::NotOwner:
            return "not_owner";
        case LeadershipStatusCode::BackendUnavailable:
            return "backend_unavailable";
        case LeadershipStatusCode::StaleMembershipEpoch:
            return "stale_membership_epoch";
        case LeadershipStatusCode::IdentityRejected:
            return "identity_rejected";
        case LeadershipStatusCode::PermissionDenied:
            return "permission_denied";
        case LeadershipStatusCode::TimedOut:
            return "timed_out";
    }
    return "unknown";
}

} // namespace hpactor::cluster::singleton
