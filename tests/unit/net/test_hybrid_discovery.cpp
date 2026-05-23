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

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

TEST(HybridDiscoveryTest, Construct) {
    RegistrarConfig reg_cfg;
    GossipConfig gossip_cfg;
    EndPoint local_ep = endpoint_ops::parse_endpoint("127.0.0.1:12345");
    HybridDiscovery hd(reg_cfg, gossip_cfg, local_ep, nullptr);
    // No crash = pass
}

TEST(HybridDiscoveryTest, DiscoverEmpty) {
    RegistrarConfig reg_cfg;
    GossipConfig gossip_cfg;
    EndPoint local_ep = endpoint_ops::parse_endpoint("127.0.0.1:12346");
    HybridDiscovery hd(reg_cfg, gossip_cfg, local_ep, nullptr);
    EndPoint remote_ep = endpoint_ops::parse_endpoint("192.168.1.1:9000");
    const auto* member = hd.discover(remote_ep);
    EXPECT_EQ(member, nullptr);
}

TEST(HybridDiscoveryTest, DiscoverAllEmpty) {
    RegistrarConfig reg_cfg;
    GossipConfig gossip_cfg;
    EndPoint local_ep = endpoint_ops::parse_endpoint("127.0.0.1:12347");
    HybridDiscovery hd(reg_cfg, gossip_cfg, local_ep, nullptr);
    auto members = hd.discover_all();
    EXPECT_TRUE(members.empty());
}

TEST(HybridDiscoveryTest, BackendName) {
    RegistrarConfig reg_cfg;
    GossipConfig gossip_cfg;
    EndPoint local_ep = endpoint_ops::parse_endpoint("127.0.0.1:12348");
    HybridDiscovery hd(reg_cfg, gossip_cfg, local_ep, nullptr);
    EXPECT_EQ(hd.backend_name(), "hybrid");
}
