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

#include <hpactor/cluster/singleton/leadership_backend_adapter.hpp>

namespace hpactor::cluster::singleton {

LeadershipBackendAdapter::LeadershipBackendAdapter(std::string self_node_id,
                                                   ILeadershipBackend* backend,
                                                   Clock::duration lease_ttl)
    : self_node_id_(std::move(self_node_id)), backend_(backend),
      lease_ttl_(lease_ttl) {}

std::optional<std::string>
LeadershipBackendAdapter::elect(const SingletonIdentity& id,
                                std::span<const std::string> /*alive_nodes*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    // alive_nodes is not used — backend owns the decision
    LeadershipAttempt attempt;
    attempt.singleton_name = id.name;
    attempt.self_node_id = self_node_id_;
    attempt.lease_ttl = lease_ttl_;

    auto result = backend_->try_acquire(attempt);

    if (result.status == LeadershipStatusCode::Granted && result.lease.has_value()) {
        leases_[id.name] = *result.lease;
        return result.lease->owner_node_id;
    }

    if (result.status == LeadershipStatusCode::AlreadyOwned) {
        if (result.lease.has_value()) {
            leases_[id.name] = *result.lease;
        }
        return result.current_owner;
    }

    if (result.status == LeadershipStatusCode::Renewed && result.lease.has_value()) {
        leases_[id.name] = *result.lease;
        return result.lease->owner_node_id;
    }

    // On any non-success path, clear stale cached lease
    leases_.erase(id.name);
    // Backend unavailable, lost, or no owner — no winner
    return std::nullopt;
}

void LeadershipBackendAdapter::on_peer_down(const std::string& /*node_id*/) {
    // Backend is the fencing authority — no local vote cleanup needed
}

uint64_t
LeadershipBackendAdapter::get_fencing_token(std::string_view singleton_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key(singleton_name);
    auto it = leases_.find(key);
    if (it == leases_.end())
        return 0;
    return it->second.fencing_token;
}

} // namespace hpactor::cluster::singleton
