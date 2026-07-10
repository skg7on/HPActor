// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cluster/singleton/leadership_lease.hpp>

namespace hpactor::cluster::singleton {

bool LeadershipLease::fences(const LeadershipLease& other) const noexcept {
    if (singleton_name != other.singleton_name)
        return false;
    if (cluster_id != other.cluster_id)
        return false;
    return fencing_token > other.fencing_token;
}

bool LeadershipLease::is_expired(Clock::time_point now) const noexcept {
    return now >= lease_deadline;
}

bool operator<(const LeadershipLease& a, const LeadershipLease& b) noexcept {
    if (a.cluster_id != b.cluster_id)
        return a.cluster_id < b.cluster_id;
    if (a.singleton_name != b.singleton_name)
        return a.singleton_name < b.singleton_name;
    return a.fencing_token < b.fencing_token;
}

bool operator==(const LeadershipLease& a, const LeadershipLease& b) noexcept {
    return a.cluster_id == b.cluster_id && a.singleton_name == b.singleton_name &&
           a.fencing_token == b.fencing_token &&
           a.owner_node_id == b.owner_node_id;
}

bool operator!=(const LeadershipLease& a, const LeadershipLease& b) noexcept {
    return !(a == b);
}

bool operator<=(const LeadershipLease& a, const LeadershipLease& b) noexcept {
    return a < b || a == b;
}

bool operator>(const LeadershipLease& a, const LeadershipLease& b) noexcept {
    return b < a;
}

} // namespace hpactor::cluster::singleton
