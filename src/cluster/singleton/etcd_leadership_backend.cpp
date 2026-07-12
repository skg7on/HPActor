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

// DESIGN NOTE: This backend uses bounded blocking (std::promise/std::future
// with configurable timeout). This is intentional for the initial
// implementation — the gRPC callbacks resolve promises on dedicated
// completion queue threads. Public methods block for at most
// cfg.request_timeout. Production hardening should move to fully async
// callbacks posting LeadershipResult messages to actor mailboxes.

#include <hpactor/cluster/singleton/etcd_leadership_backend.hpp>
#include <hpactor/cluster/singleton/leadership_lease.hpp>
#include <hpactor/cluster/singleton/leadership_status.hpp>

#include <future>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::singleton {

namespace {

/// rief Convert etcd mod_revision to fencing token.
/// etcd revisions are globally monotonic — directly usable as fencing tokens.
[[maybe_unused]] uint64_t revision_to_token(int64_t mod_revision) {
    return static_cast<uint64_t>(mod_revision);
}

} // namespace

struct EtcdLeadershipBackend::Impl {
    Config cfg;
    // per-singleton state: active lease_id, watch handle, keepalive handle
    struct SingletonState {
        int64_t lease_id = 0;
        LeadershipLease current_lease;
        bool keepalive_active = false;
        bool watch_active = false;
    };
    std::unordered_map<std::string, SingletonState> singletons_;
    std::mutex mutex_;
};

EtcdLeadershipBackend::EtcdLeadershipBackend(Config cfg)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
}

EtcdLeadershipBackend::~EtcdLeadershipBackend() = default;

LeadershipResult
EtcdLeadershipBackend::try_acquire(const LeadershipAttempt& /*attempt*/) {
    // 1. Grant etcd lease with TTL
    // 2. Build Txn: if owner key absent -> put; else return current
    // 3. Execute Txn
    // 4. If success: extract mod_revision -> fencing_token, start keepalive +
    // watch
    // 5. If key exists: return AlreadyOwned with current owner
    // 6. On error/timeout: return BackendUnavailable or TimedOut

    // For the initial implementation, the bounded blocking pattern:
    auto promise = std::make_shared<std::promise<LeadershipResult>>();
    auto future = promise->get_future();

    // TODO: actual gRPC calls go here
    // lease_client_->grant(ttl, [promise](auto lease_id, auto status) { ... });

    auto status = future.wait_for(impl_->cfg.request_timeout);
    if (status == std::future_status::timeout) {
        return LeadershipResult::timed_out();
    }
    return future.get();
}

LeadershipResult EtcdLeadershipBackend::renew(const LeadershipLease& /*lease*/) {
    // 1. Verify lease.owner_node_id matches local identity
    // 2. KeepAlive heartbeat sends (bidirectional stream)
    // 3. Txn: verify owner key still points to this node + token
    // 4. Return renewed lease with updated revision as new fencing_token
    return LeadershipResult::unavailable(); // stub — backend not connected
}

LeadershipResult EtcdLeadershipBackend::release(const LeadershipLease& /*lease*/) {
    // 1. Txn: delete owner key only if value matches local identity + token
    // 2. Revoke lease
    // 3. Cancel KeepAlive and Watch streams
    return LeadershipResult::unavailable(); // stub — backend not connected
}

LeadershipResult
EtcdLeadershipBackend::current_owner(std::string_view /*singleton_name*/) {
    // 1. Range query on owner key
    // 2. If key exists: deserialize -> return Granted with lease
    // 3. If key absent: return NotOwner
    return LeadershipResult{LeadershipStatusCode::NotOwner, std::nullopt,
                            std::nullopt};
}

} // namespace hpactor::cluster::singleton
