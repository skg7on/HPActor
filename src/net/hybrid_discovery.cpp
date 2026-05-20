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

#include <hpactor/net/hybrid_discovery.hpp>

#include <unordered_map>

namespace hpactor::net {

HybridDiscovery::HybridDiscovery(const RegistrarConfig& reg_cfg,
                                 const GossipConfig& gossip_cfg,
                                 EndPoint local_ep, EventLoop* loop)
    : registrar_(reg_cfg, local_ep, loop), gossip_(gossip_cfg, loop),
      local_ep_(local_ep) {}

HybridDiscovery::~HybridDiscovery() {
    stop();
}

void HybridDiscovery::start() {
    registrar_.start(); // first: determines server/client mode
    gossip_.start();    // second: cross-host gossip
}

void HybridDiscovery::stop() {
    gossip_.stop();    // first: graceful Leave to peers
    registrar_.stop(); // second: close registrar sockets
}

std::vector<Member> HybridDiscovery::discover_all() const {
    auto local = registrar_.discover_all();
    auto remote = gossip_.discover_all();
    // Same-host entries take precedence on collision
    std::unordered_map<EndPoint, Member> merged;
    for (auto& m : remote)
        merged[m.identity.endpoint] = std::move(m);
    for (auto& m : local)
        merged[m.identity.endpoint] = std::move(m);
    std::vector<Member> result;
    result.reserve(merged.size());
    for (auto& [_, m] : merged)
        result.push_back(std::move(m));
    return result;
}

const Member* HybridDiscovery::discover(EndPoint ep) const {
    auto* local = registrar_.discover(ep);
    if (local)
        return local;
    return gossip_.discover(ep);
}

void HybridDiscovery::announce(Member m) {
    // Delegate to registrar if local endpoint
    if (m.identity.endpoint == local_ep_ || m.identity.host == "127.0.0.1") {
        registrar_.announce(m);
    }
    // Always propagate via gossip for cross-host visibility
    gossip_.announce(std::move(m));
}

void HybridDiscovery::on_member_change(MemberChangeCallback cb) {
    user_callback_ = std::move(cb);
    // Cross-host events forwarded directly
    gossip_.on_member_change(user_callback_);
    // Same-host events piped through on_local_member_change
    registrar_.on_member_change([this](const Member& m, bool joined) {
        on_local_member_change(m, joined);
    });
}

void HybridDiscovery::on_local_member_change(const Member& m, bool joined) {
    // Push local changes into gossip layer for cross-host visibility
    gossip_.announce(m);
    if (user_callback_)
        user_callback_(m, joined);
}

} // namespace hpactor::net
