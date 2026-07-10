// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

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
