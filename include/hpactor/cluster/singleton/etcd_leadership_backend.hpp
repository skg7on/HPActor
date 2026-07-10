// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_backend.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {
namespace cluster::singleton {

/// \brief Production etcd-backed leadership backend.
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
