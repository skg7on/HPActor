// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_backend.hpp>
#include <hpactor/cluster/singleton/singleton_election.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::singleton {

/// \brief Adapts an ILeadershipBackend to the ISingletonElection interface.
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
    LeadershipBackendAdapter(std::string self_node_id, ILeadershipBackend* backend);

    std::optional<std::string>
    elect(const SingletonIdentity& id,
          std::span<const std::string> alive_nodes) override;

    void on_peer_down(const std::string& node_id) override;

    /// \brief Return the backend-issued fencing token for a singleton.
    uint64_t get_fencing_token(std::string_view singleton_name) const override;

  private:
    std::string self_node_id_;
    ILeadershipBackend* backend_;
    std::unordered_map<std::string, LeadershipLease> leases_;
};

} // namespace hpactor::cluster::singleton
