// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_lease.hpp>
#include <hpactor/cluster/singleton/leadership_status.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace hpactor::cluster::singleton {

/// \brief Parameters for a leadership acquisition attempt.
struct LeadershipAttempt {
    std::string singleton_name;
    std::string self_node_id;
    uint64_t observed_membership_epoch = 0;
    Clock::duration lease_ttl{};
};

/// \brief Abstract backend for distributed singleton leadership.
///
/// Implementations provide linearizable ownership through etcd, Consul,
/// or internal Raft. All calls return explicit LeadershipResult values.
/// No exception-based control flow. Backend implementations run on
/// dedicated executors — never on scheduler or event-loop hot paths.
class ILeadershipBackend {
  public:
    virtual ~ILeadershipBackend() = default;

    /// \brief Attempt to acquire leadership for a singleton.
    virtual LeadershipResult try_acquire(const LeadershipAttempt& attempt) = 0;

    /// \brief Renew an existing lease before it expires.
    virtual LeadershipResult renew(const LeadershipLease& lease) = 0;

    /// \brief Voluntarily release a lease.
    virtual LeadershipResult release(const LeadershipLease& lease) = 0;

    /// \brief Query the current owner of a singleton.
    virtual LeadershipResult current_owner(std::string_view singleton_name) = 0;
};

} // namespace hpactor::cluster::singleton
