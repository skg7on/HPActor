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

#include <hpactor/cluster/singleton/fake_leadership_backend.hpp>

namespace hpactor::cluster::singleton {

LeadershipResult
FakeLeadershipBackend::try_acquire(const LeadershipAttempt& attempt) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (unavailable_)
        return LeadershipResult::unavailable();

    auto it = leases_.find(attempt.singleton_name);
    if (it == leases_.end()) {
        // No pre-configured entry for this singleton — not acquirable
        return {LeadershipStatusCode::NotOwner, std::nullopt, std::nullopt};
    }

    if (it->second.owned) {
        // Already owned — check if it's us
        if (it->second.lease.owner_node_id == attempt.self_node_id) {
            // Idempotent — return existing lease
            LeadershipResult r;
            r.status = LeadershipStatusCode::Granted;
            r.lease = it->second.lease;
            return r;
        }
        return LeadershipResult::already_owned(it->second.lease.owner_node_id,
                                               it->second.lease);
    }

    // Entry exists but not owned — acquire and grant new lease
    LeadershipLease lease;
    lease.cluster_id = "fake";
    lease.singleton_name = attempt.singleton_name;
    lease.owner_node_id = attempt.self_node_id;
    lease.membership_epoch = attempt.observed_membership_epoch;
    lease.fencing_token = next_token_++;
    lease.backend_revision = lease.fencing_token;
    lease.lease_deadline = Clock::now() + attempt.lease_ttl;

    it->second.lease = lease;
    it->second.original_ttl = attempt.lease_ttl;
    it->second.owned = true;

    return LeadershipResult::granted(lease);
}

LeadershipResult FakeLeadershipBackend::renew(const LeadershipLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (unavailable_)
        return LeadershipResult::unavailable();

    auto it = leases_.find(lease.singleton_name);
    if (it == leases_.end() || !it->second.owned) {
        return LeadershipResult::lost();
    }
    if (it->second.lease.owner_node_id != lease.owner_node_id) {
        return LeadershipResult::lost();
    }

    // Bump token and extend deadline using the original TTL
    it->second.lease.fencing_token = next_token_++;
    it->second.lease.backend_revision = it->second.lease.fencing_token;
    it->second.lease.lease_deadline = Clock::now() + it->second.original_ttl;
    return LeadershipResult::renewed(it->second.lease);
}

LeadershipResult FakeLeadershipBackend::release(const LeadershipLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (unavailable_)
        return LeadershipResult::unavailable();

    auto it = leases_.find(lease.singleton_name);
    if (it == leases_.end() || !it->second.owned) {
        return LeadershipResult::released(); // idempotent
    }
    if (it->second.lease.owner_node_id != lease.owner_node_id) {
        return LeadershipResult::lost(); // not our lease
    }

    it->second.owned = false;
    return LeadershipResult::released();
}

LeadershipResult
FakeLeadershipBackend::current_owner(std::string_view singleton_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (unavailable_)
        return LeadershipResult::unavailable();

    std::string key(singleton_name);
    auto it = leases_.find(key);
    if (it == leases_.end() || !it->second.owned) {
        return {LeadershipStatusCode::NotOwner, std::nullopt, std::nullopt};
    }
    LeadershipResult r;
    r.status = LeadershipStatusCode::Granted;
    r.lease = it->second.lease;
    return r;
}

void FakeLeadershipBackend::force_grant(const std::string& singleton_name,
                                        const std::string& owner_node_id,
                                        Clock::duration ttl) {
    std::lock_guard<std::mutex> lock(mutex_);
    LeadershipLease lease;
    lease.cluster_id = "fake";
    lease.singleton_name = singleton_name;
    lease.owner_node_id = owner_node_id;
    lease.fencing_token = next_token_++;
    lease.lease_deadline = Clock::now() + ttl;
    StoredLease stored;
    stored.lease = lease;
    stored.owned = true;
    stored.original_ttl = ttl;
    leases_[singleton_name] = stored;
}

void FakeLeadershipBackend::force_revoke(const std::string& singleton_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    leases_.erase(singleton_name);
}

void FakeLeadershipBackend::simulate_unavailable(bool unavailable) {
    std::lock_guard<std::mutex> lock(mutex_);
    unavailable_ = unavailable;
}

int FakeLeadershipBackend::get_grant_count(const std::string& singleton_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = leases_.find(singleton_name);
    if (it == leases_.end())
        return 0;
    return it->second.owned ? 1 : 0;
}

} // namespace hpactor::cluster::singleton
