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
           a.fencing_token == b.fencing_token;
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
