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

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {
namespace cluster::singleton {

/// rief Production etcd-backed leadership backend.
///
/// Implements ILeadershipBackend using etcd v3 gRPC API.
/// gRPC completion queue runs on dedicated threads — callbacks
/// resolve std::promise objects, and public methods block on futures
/// with bounded timeouts. No exceptions cross the boundary into
/// actor code.
class EtcdLeadershipBackend : public ILeadershipBackend {
  public:
    struct Config {
        std::vector<std::string> endpoints;
        std::string key_prefix = "/hpactor";
        std::chrono::milliseconds request_timeout{1000};
    };

    explicit EtcdLeadershipBackend(Config cfg);
    ~EtcdLeadershipBackend() override;

    // ── ILeadershipBackend ──────────────────────────────────────────

    LeadershipResult try_acquire(const LeadershipAttempt& attempt) override;
    LeadershipResult renew(const LeadershipLease& lease) override;
    LeadershipResult release(const LeadershipLease& lease) override;
    LeadershipResult current_owner(std::string_view singleton_name) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cluster::singleton
} // namespace hpactor
