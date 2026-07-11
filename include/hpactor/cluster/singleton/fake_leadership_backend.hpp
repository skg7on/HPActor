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

#include <hpactor/cluster/singleton/leadership_backend.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::singleton {

/// rief Deterministic fake ILeadershipBackend for unit testing.
///
/// Test code pre-configures ownership via force_grant() and calls
/// simulate_unavailable() to exercise failure paths. Not thread-safe
/// by default — callers must serialize access in multi-threaded tests.
class FakeLeadershipBackend : public ILeadershipBackend {
  public:
    FakeLeadershipBackend() = default;

    // ── ILeadershipBackend ──────────────────────────────────────────

    LeadershipResult try_acquire(const LeadershipAttempt& attempt) override;
    LeadershipResult renew(const LeadershipLease& lease) override;
    LeadershipResult release(const LeadershipLease& lease) override;
    LeadershipResult current_owner(std::string_view singleton_name) override;

    // ── Test control surface ────────────────────────────────────────

    /// rief Pre-configure ownership so the next try_acquire succeeds.
    void force_grant(const std::string& singleton_name,
                     const std::string& owner_node_id, Clock::duration ttl);

    /// rief Force-remove ownership for a singleton.
    void force_revoke(const std::string& singleton_name);

    /// rief Toggle backend unavailability for all operations.
    void simulate_unavailable(bool unavailable);

    /// rief Number of acquire attempts for a singleton.
    int get_grant_count(const std::string& singleton_name) const;

  private:
    struct StoredLease {
        LeadershipLease lease;
        bool owned = false;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StoredLease> leases_;
    uint64_t next_token_ = 1;
    bool unavailable_ = false;
};

} // namespace hpactor::cluster::singleton
