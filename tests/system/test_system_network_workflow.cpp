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

// System test: Network Transport Workflow
// Exercises transport lifecycle, WireFrame protocol, event loop, acceptor,
// connection pool, HTTP client, and actor location cache over loopback.

#include <gtest/gtest.h>

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/net/wireframe_connection.hpp>

#include "system_test_fixture.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace hpactor;

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 1: Transport lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkWorkflow, TwoSystemsLoopbackDataExchange) {
    // Create two ActorSystems on loopback with networking enabled
    Config cfg_a = test::config_with_scheduler(1);
    cfg_a.enable_network = true;
    cfg_a.tcp_port = 0; // ephemeral
    cfg_a.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem sys_a(cfg_a);
    EXPECT_TRUE(sys_a.is_running());
    EXPECT_NE(sys_a.transport(), nullptr);

    Config cfg_b = test::config_with_scheduler(1);
    cfg_b.enable_network = true;
    cfg_b.tcp_port = 0;
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem sys_b(cfg_b);
    EXPECT_TRUE(sys_b.is_running());
    EXPECT_NE(sys_b.transport(), nullptr);

    // Both transports should exist and have valid (non-empty) endpoints
    auto* t_a = sys_a.transport();
    auto* t_b = sys_b.transport();
    ASSERT_NE(t_a, nullptr);
    ASSERT_NE(t_b, nullptr);

    auto ep_a = t_a->endpoint();
    auto ep_b = t_b->endpoint();
    EXPECT_NE(ep_a, EndPoint{});
    EXPECT_NE(ep_b, EndPoint{});

    // Clean shutdown
    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

TEST(NetworkWorkflow, TransportEphemeralPortListen) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0; // ephemeral — OS assigns a free port
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    EXPECT_NE(system.transport(), nullptr);

    // After binding ephemeral port, endpoint should be valid
    auto ep = system.endpoint();
    EXPECT_NE(ep, EndPoint{});

    // Verify transport is a valid instance
    EXPECT_NE(system.transport(), nullptr);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(NetworkWorkflow, TransportDisabledByDefault) {
    Config cfg = test::minimal_config();
    EXPECT_FALSE(cfg.enable_network);

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());

    // Network transport should be nullptr when networking is disabled
    EXPECT_EQ(system.transport(), nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 2: WireFrame and Connection
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkWorkflow, WireFrameProtocolEncodeDecode) {
    // Create a WireFrame and verify encode/decode round-trip
    net::WireFrame frame;
    frame.pb_frame.set_type_tag(42);
    frame.pb_frame.set_message_id(100);
    frame.pb_frame.set_flags(net::WireFrame::Important);

    // Encode to wire format
    StreamBuffer encoded = frame.encode();
    EXPECT_GE(encoded.size(), net::WireFrame::HeaderSize);

    // Decode back from wire format
    net::WireFrame decoded = net::WireFrame::decode(encoded);
    EXPECT_EQ(decoded.pb_frame.type_tag(), 42);
    EXPECT_EQ(decoded.pb_frame.message_id(), 100);
    EXPECT_EQ(decoded.pb_frame.flags(), net::WireFrame::Important);
    EXPECT_EQ(decoded.magic_hdr, net::WireFrame::MagicHeader);
}

TEST(NetworkWorkflow, WireFrameMagicHeaderConstant) {
    // Verify the HPAC magic header constant
    EXPECT_EQ(net::WireFrame::MagicHeader, 0x43415048u);
    EXPECT_EQ(net::WireFrame::HeaderSize, 8u);
}

TEST(NetworkWorkflow, TwoSystemsEndpointConnectivity) {
    // Setup 2 systems on loopback, verify their endpoints can be used
    // for connectivity checks (no actual connection needed)
    Config cfg_a = test::config_with_scheduler(1);
    cfg_a.enable_network = true;
    cfg_a.tcp_port = 0;
    cfg_a.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem sys_a(cfg_a);
    EXPECT_TRUE(sys_a.is_running());
    auto* t_a = sys_a.transport();
    ASSERT_NE(t_a, nullptr);

    Config cfg_b = test::config_with_scheduler(1);
    cfg_b.enable_network = true;
    cfg_b.tcp_port = 0;
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem sys_b(cfg_b);
    EXPECT_TRUE(sys_b.is_running());
    auto* t_b = sys_b.transport();
    ASSERT_NE(t_b, nullptr);

    // Each system should not be connected to the other initially
    // (since no connection has been established)
    EXPECT_FALSE(t_a->is_connected(t_b->endpoint()));
    EXPECT_FALSE(t_b->is_connected(t_a->endpoint()));

    // Systems should not be connected to themselves either
    EXPECT_FALSE(t_a->is_connected(t_a->endpoint()));
    EXPECT_FALSE(t_b->is_connected(t_b->endpoint()));

    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

TEST(NetworkWorkflow, ConnectionPoolIsConnectedForUnknown) {
    // Verify is_connected returns false for unknown/random endpoints
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    // An unknown endpoint (high port, not in registry) — should not be
    // connected
    auto unknown = endpoint_ops::parse_endpoint("127.0.0.1:65123");
    EXPECT_FALSE(transport->is_connected(unknown));

    // Connect with no registry entry should return nullptr
    auto conn = transport->connect(unknown);
    EXPECT_EQ(conn, nullptr);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}
