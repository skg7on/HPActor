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

#include <hpactor/cluster/singleton/leadership_lease.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace hpactor::cluster::singleton {

/// rief Outcome codes for ILeadershipBackend operations.
enum class LeadershipStatusCode : uint8_t {
    Granted,
    AlreadyOwned,
    Renewed,
    Released,
    Lost,
    NotOwner,
    BackendUnavailable,
    StaleMembershipEpoch,
    IdentityRejected,
    PermissionDenied,
    TimedOut,
};

/// rief Human-readable snake_case string for the status code.
const char* to_string(LeadershipStatusCode code) noexcept;

/// rief Result of a leadership backend operation.
///
/// Carries the lease on success (Granted, Renewed) and the current
/// owner on AlreadyOwned. Lost, BackendUnavailable, and other failures
/// carry no lease.
struct LeadershipResult {
    LeadershipStatusCode status = LeadershipStatusCode::NotOwner;
    std::optional<LeadershipLease> lease;
    std::optional<std::string> current_owner;

    static LeadershipResult granted(LeadershipLease l) {
        LeadershipResult r;
        r.status = LeadershipStatusCode::Granted;
        r.lease = std::move(l);
        return r;
    }
    static LeadershipResult renewed(LeadershipLease l) {
        LeadershipResult r;
        r.status = LeadershipStatusCode::Renewed;
        r.lease = std::move(l);
        return r;
    }
    static LeadershipResult already_owned(std::string owner, LeadershipLease l) {
        LeadershipResult r;
        r.status = LeadershipStatusCode::AlreadyOwned;
        r.current_owner = std::move(owner);
        r.lease = std::move(l);
        return r;
    }
    static LeadershipResult released() {
        return {LeadershipStatusCode::Released, std::nullopt, std::nullopt};
    }
    static LeadershipResult lost() {
        return {LeadershipStatusCode::Lost, std::nullopt, std::nullopt};
    }
    static LeadershipResult unavailable() {
        return {LeadershipStatusCode::BackendUnavailable, std::nullopt,
                std::nullopt};
    }
    static LeadershipResult stale_epoch() {
        return {LeadershipStatusCode::StaleMembershipEpoch, std::nullopt,
                std::nullopt};
    }
    static LeadershipResult identity_rejected() {
        return {LeadershipStatusCode::IdentityRejected, std::nullopt, std::nullopt};
    }
    static LeadershipResult timed_out() {
        return {LeadershipStatusCode::TimedOut, std::nullopt, std::nullopt};
    }

    [[nodiscard]] bool is_success() const noexcept {
        return status == LeadershipStatusCode::Granted ||
               status == LeadershipStatusCode::Renewed ||
               status == LeadershipStatusCode::Released;
    }
};

} // namespace hpactor::cluster::singleton
