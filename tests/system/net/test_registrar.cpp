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

#include <hpactor/net/registrar.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

TEST(RegistrarTest, AcceptorInfoDefaults) {
    AcceptorInfo info;
    EXPECT_EQ(info.port, 0u);
    EXPECT_EQ(info.handshake_version, 0u);
    EXPECT_EQ(info.protocol_version, 0u);
    EXPECT_FALSE(info.tls_required);
}

TEST(RegistrarTest, RegistrarConfigDefaults) {
    RegistrarConfig config;
    EXPECT_EQ(config.udp_port, 5353u);
    EXPECT_EQ(config.tcp_port, 5353u);
    EXPECT_EQ(config.heartbeat_interval.count(), 5000);
    EXPECT_EQ(config.expiration_timeout.count(), 15000);
    EXPECT_EQ(config.probe_interval.count(), 30000);
    EXPECT_TRUE(config.static_routes.empty());
    EXPECT_FALSE(config.disable_server);
}

TEST(RegistrarTest, StaticRouteConfigDefaults) {
    StaticRouteConfig route;
    EXPECT_EQ(route.endpoint, EndPoint{});
    EXPECT_EQ(route.port, 0u);
}

TEST(RegistrarTest, NodeEndpointDefaults) {
    NodeEndpoint ep;
    EXPECT_EQ(ep.identity.endpoint, EndPoint{});
    EXPECT_EQ(ep.tcp_port, 0u);
    EXPECT_FALSE(ep.is_static_route);
    EXPECT_TRUE(ep.identity.acceptors.empty());
}

TEST(RegistrarTest, NodeEndpointWithAcceptors) {
    NodeEndpoint ep;
    ep.identity.endpoint = hpactor::endpoint_ops::parse_endpoint("node1:12345");
    ep.identity.host = "localhost";
    ep.tcp_port = 9000;
    ep.identity.acceptors.push_back({9000, 1, 1, false});
    EXPECT_EQ(ep.identity.acceptors.size(), 1u);
    EXPECT_EQ(ep.identity.acceptors[0].port, 9000u);
    EXPECT_EQ(ep.identity.acceptors[0].handshake_version, 1u);
    EXPECT_EQ(ep.identity.acceptors[0].protocol_version, 1u);
    EXPECT_FALSE(ep.identity.acceptors[0].tls_required);
}

TEST(RegistrarTest, HostResolverCache) {
    HostResolver resolver;
    resolver.cache("192.168.1.1", "192.168.1.1", std::chrono::seconds(60));
    EXPECT_EQ(resolver.get_cached("192.168.1.1"), "192.168.1.1");
    EXPECT_EQ(resolver.get_cached("unknown"), "");
}

TEST(RegistrarTest, RegistrarMessageTypeEnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(RegistrarMessageType::Register), 0x01);
    EXPECT_EQ(static_cast<uint8_t>(RegistrarMessageType::Heartbeat), 0x02);
    EXPECT_EQ(static_cast<uint8_t>(RegistrarMessageType::NodeJoin), 0x03);
    EXPECT_EQ(static_cast<uint8_t>(RegistrarMessageType::NodeLeave), 0x04);
    EXPECT_EQ(static_cast<uint8_t>(RegistrarMessageType::Accept), 0x05);
    EXPECT_EQ(static_cast<uint8_t>(RegistrarMessageType::Error), 0x06);
    EXPECT_EQ(static_cast<uint8_t>(RegistrarMessageType::ResolveQuery), 0x10);
    EXPECT_EQ(static_cast<uint8_t>(RegistrarMessageType::ResolveResponse), 0x11);
}

TEST(RegistrarTest, NodeRegistry) {
    RegistrarConfig reg_config;
    NodeRegistry registry(reg_config);
    EXPECT_TRUE(registry.all().empty());

    NodeEndpoint ep2;
    ep2.identity.endpoint = hpactor::endpoint_ops::parse_endpoint("node42:"
                                                                  "12345");
    ep2.identity.host = "192.168.1.100";
    ep2.tcp_port = 9001;
    ep2.is_static_route = true;
    registry.upsert_endpoint(ep2);

    EXPECT_TRUE(registry.has(hpactor::endpoint_ops::parse_endpoint("node42:"
                                                                   "12345")));
    EXPECT_EQ(
        registry.get(hpactor::endpoint_ops::parse_endpoint("node42:12345"))->tcp_port,
        9001u);
    EXPECT_EQ(registry.all().size(), 1u);
}
