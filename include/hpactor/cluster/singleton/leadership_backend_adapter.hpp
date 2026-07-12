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
#include <hpactor/cluster/singleton/singleton_election.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::singleton {

/// rief Adapts an ILeadershipBackend to the ISingletonElection interface.
///
/// Each call to elect() internally calls ILeadershipBackend::try_acquire().
/// The adapter caches the latest lease per singleton and exposes the
/// backend-issued fencing token via get_fencing_token().
///
/// on_peer_down() is a no-op — the backend is the fencing authority.
class LeadershipBackendAdapter : public ISingletonElection {
  public:
    /// \param[in] self_node_id This node's identity.
    /// \param[in] backend The leadership backend (owned by caller).
    /// \param[in] lease_ttl Lease TTL for leadership attempts.
    LeadershipBackendAdapter(std::string self_node_id, ILeadershipBackend* backend,
                             Clock::duration lease_ttl = std::chrono::seconds(10));

    std::optional<std::string>
    elect(const SingletonIdentity& id,
          std::span<const std::string> alive_nodes) override;

    void on_peer_down(const std::string& node_id) override;

    /// rief Return the backend-issued fencing token for a singleton.
    uint64_t get_fencing_token(std::string_view singleton_name) const override;

  private:
    std::string self_node_id_;
    ILeadershipBackend* backend_;
    Clock::duration lease_ttl_{std::chrono::seconds(10)};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, LeadershipLease> leases_;
};

} // namespace hpactor::cluster::singleton
