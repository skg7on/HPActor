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

#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/gossip_membership.hpp>

namespace hpactor::net {

class HybridDiscovery : public IServiceDiscovery {
public:
    HybridDiscovery(const RegistrarConfig& reg_cfg, const GossipConfig& gossip_cfg,
                    EndPoint local_ep, EventLoop* loop);
    ~HybridDiscovery() override;

    void start() override;
    void stop() override;
    std::vector<Member> discover_all() const override;
    const Member* discover(EndPoint) const override;
    void announce(Member) override;
    void on_member_change(MemberChangeCallback) override;
    std::string backend_name() const override { return "hybrid"; }

private:
    void on_local_member_change(const Member& m, bool joined);

    UdpRegistrar registrar_;
    GossipMembership gossip_;
    EndPoint local_ep_;
    MemberChangeCallback user_callback_;
};

} // namespace hpactor::net
