// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor::cluster::singleton {

using Clock = std::chrono::steady_clock;

/// \brief A backend-issued leadership lease for a cluster singleton.
///
/// Grants exactly-one ownership semantics. The fencing token is monotonically
/// ordered by the backend for a given (cluster_id, singleton_name) pair.
/// Consumers compare fencing_token to reject stale commands.
struct LeadershipLease {
    std::string cluster_id;
    std::string singleton_name;
    std::string owner_node_id;
    uint64_t owner_incarnation = 0;
    uint64_t owner_process_start_id = 0;
    uint64_t membership_epoch = 0;
    uint64_t fencing_token = 0;
    uint64_t backend_term = 0;
    uint64_t backend_revision = 0;
    Clock::time_point lease_deadline{};

    /// \brief True if this lease's fencing token strictly dominates \p other
    /// for the same singleton.
    [[nodiscard]] bool fences(const LeadershipLease& other) const noexcept;

    /// \brief True if the lease deadline has passed.
    [[nodiscard]] bool is_expired(Clock::time_point now) const noexcept;

    friend bool
    operator<(const LeadershipLease& a, const LeadershipLease& b) noexcept;
    friend bool
    operator==(const LeadershipLease& a, const LeadershipLease& b) noexcept;
    friend bool
    operator!=(const LeadershipLease& a, const LeadershipLease& b) noexcept;
    friend bool
    operator<=(const LeadershipLease& a, const LeadershipLease& b) noexcept;
    friend bool
    operator>(const LeadershipLease& a, const LeadershipLease& b) noexcept;
};

} // namespace hpactor::cluster::singleton
