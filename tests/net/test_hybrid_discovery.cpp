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

#include <cassert>
#include <cstdio>
#include <hpactor/net/hybrid_discovery.hpp>

using namespace hpactor;
using namespace hpactor::net;

void test_construct() {
    RegistrarConfig reg_cfg;
    GossipConfig gossip_cfg;
    EndPoint local_ep = endpoint_ops::parse_endpoint("127.0.0.1:12345");
    HybridDiscovery hd(reg_cfg, gossip_cfg, local_ep, nullptr);
    printf("  PASSED test_construct\n");
}

void test_discover_empty() {
    RegistrarConfig reg_cfg;
    GossipConfig gossip_cfg;
    EndPoint local_ep = endpoint_ops::parse_endpoint("127.0.0.1:12346");
    HybridDiscovery hd(reg_cfg, gossip_cfg, local_ep, nullptr);
    EndPoint remote_ep = endpoint_ops::parse_endpoint("192.168.1.1:9000");
    const auto* member = hd.discover(remote_ep);
    assert(member == nullptr);
    printf("  PASSED test_discover_empty\n");
}

void test_discover_all_empty() {
    RegistrarConfig reg_cfg;
    GossipConfig gossip_cfg;
    EndPoint local_ep = endpoint_ops::parse_endpoint("127.0.0.1:12347");
    HybridDiscovery hd(reg_cfg, gossip_cfg, local_ep, nullptr);
    auto members = hd.discover_all();
    assert(members.empty());
    printf("  PASSED test_discover_all_empty\n");
}

void test_backend_name() {
    RegistrarConfig reg_cfg;
    GossipConfig gossip_cfg;
    EndPoint local_ep = endpoint_ops::parse_endpoint("127.0.0.1:12348");
    HybridDiscovery hd(reg_cfg, gossip_cfg, local_ep, nullptr);
    assert(hd.backend_name() == "hybrid");
    printf("  PASSED test_backend_name\n");
}

int main() {
    printf("HybridDiscovery tests:\n");
    test_construct();
    test_discover_empty();
    test_discover_all_empty();
    test_backend_name();
    printf("All HybridDiscovery tests PASSED\n");
    return 0;
}
