// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cluster/singleton/leadership_backend_adapter.hpp>

namespace hpactor::cluster::singleton {

LeadershipBackendAdapter::LeadershipBackendAdapter(std::string self_node_id,
                                                   ILeadershipBackend* backend)
    : self_node_id_(std::move(self_node_id)), backend_(backend) {}

std::optional<std::string>
LeadershipBackendAdapter::elect(const SingletonIdentity& id,
                                std::span<const std::string> /*alive_nodes*/) {
    // alive_nodes is not used — backend owns the decision
    LeadershipAttempt attempt;
    attempt.singleton_name = id.name;
    attempt.self_node_id = self_node_id_;
    attempt.lease_ttl = std::chrono::seconds(10); // default TTL, configurable
                                                  // later

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

    // Backend unavailable, lost, or no owner — no winner
    return std::nullopt;
}

void LeadershipBackendAdapter::on_peer_down(const std::string& /*node_id*/) {
    // Backend is the fencing authority — no local vote cleanup needed
}

uint64_t
LeadershipBackendAdapter::get_fencing_token(std::string_view singleton_name) const {
    std::string key(singleton_name);
    auto it = leases_.find(key);
    if (it == leases_.end())
        return 0;
    return it->second.fencing_token;
}

} // namespace hpactor::cluster::singleton
