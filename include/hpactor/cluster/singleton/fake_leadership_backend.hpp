// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_backend.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::singleton {

/// \brief Deterministic fake ILeadershipBackend for unit testing.
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

    /// \brief Pre-configure ownership so the next try_acquire succeeds.
    void force_grant(const std::string& singleton_name,
                     const std::string& owner_node_id, Clock::duration ttl);

    /// \brief Force-remove ownership for a singleton.
    void force_revoke(const std::string& singleton_name);

    /// \brief Toggle backend unavailability for all operations.
    void simulate_unavailable(bool unavailable);

    /// \brief Number of acquire attempts for a singleton.
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
