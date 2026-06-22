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

#include <hpactor/net/gossip_membership.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/service_discovery.hpp>

namespace hpactor::net {

/// \brief Hybrid discovery backend — composes UDP registrar and SWIM gossip.
///
/// Runs both \c UdpRegistrar (for same-host discovery) and
/// \c GossipMembership (for cross-server discovery) simultaneously.
/// Member changes from either backend are forwarded to the registered
/// user callback.
///
/// \note Thread safety: Delegates to the thread-safety guarantees of the
///       composed backends.
class HybridDiscovery : public IServiceDiscovery {
  public:
    /// \brief Construct a hybrid discovery instance.
    ///
    /// \param[in] reg_cfg Registrar configuration.
    /// \param[in] gossip_cfg Gossip protocol configuration.
    /// \param[in] local_ep This node's endpoint.
    /// \param[in] loop Event loop for async I/O.
    HybridDiscovery(const RegistrarConfig& reg_cfg, const GossipConfig& gossip_cfg,
                    EndPoint local_ep, EventLoop* loop);
    ~HybridDiscovery() override;

    /// \brief Start both backends.
    void start() override;

    /// \brief Stop both backends.
    void stop() override;

    /// \brief Discover all members from both backends (deduplicated).
    ///
    /// \return Merged member list with duplicates removed by endpoint.
    std::vector<Member> discover_all() const override;

    /// \brief Look up a member by endpoint in either backend.
    ///
    /// \param[in] ep Endpoint to search for.
    /// \return Pointer to the member, or \c nullptr if not found.
    const Member* discover(EndPoint ep) const override;

    /// \brief Announce local state to both backends.
    ///
    /// \param[in] m Local member state.
    void announce(Member m) override;

    /// \brief Register a callback for membership changes from either backend.
    ///
    /// \param[in] cb Callback invoked on join/leave.
    void on_member_change(MemberChangeCallback cb) override;

    std::string backend_name() const override {
        return "hybrid";
    }

  private:
    /// \brief Forward member changes from either backend to the user.
    ///
    /// \param[in] m Member that changed.
    /// \param[in] joined \c true if joined, \c false if left.
    void on_local_member_change(const Member& m, bool joined);

    UdpRegistrar registrar_;
    GossipMembership gossip_;
    EndPoint local_ep_;
    MemberChangeCallback user_callback_;
};

} // namespace hpactor::net
