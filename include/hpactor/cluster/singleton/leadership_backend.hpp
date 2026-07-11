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

#include <hpactor/cluster/singleton/leadership_lease.hpp>
#include <hpactor/cluster/singleton/leadership_status.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace hpactor::cluster::singleton {

/// rief Parameters for a leadership acquisition attempt.
struct LeadershipAttempt {
    std::string singleton_name;
    std::string self_node_id;
    uint64_t observed_membership_epoch = 0;
    Clock::duration lease_ttl{};
};

/// rief Abstract backend for distributed singleton leadership.
///
/// Implementations provide linearizable ownership through etcd, Consul,
/// or internal Raft. All calls return explicit LeadershipResult values.
/// No exception-based control flow. Backend implementations run on
/// dedicated executors — never on scheduler or event-loop hot paths.
class ILeadershipBackend {
  public:
    virtual ~ILeadershipBackend() = default;

    /// rief Attempt to acquire leadership for a singleton.
    virtual LeadershipResult try_acquire(const LeadershipAttempt& attempt) = 0;

    /// rief Renew an existing lease before it expires.
    virtual LeadershipResult renew(const LeadershipLease& lease) = 0;

    /// rief Voluntarily release a lease.
    virtual LeadershipResult release(const LeadershipLease& lease) = 0;

    /// rief Query the current owner of a singleton.
    virtual LeadershipResult current_owner(std::string_view singleton_name) = 0;
};

} // namespace hpactor::cluster::singleton
