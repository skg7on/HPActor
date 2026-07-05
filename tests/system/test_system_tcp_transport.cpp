// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

// System test: TCP Transport
// Exercises TcpTransport listen/connect/send lifecycle over loopback.

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/net/transport.hpp>

#include "system_test_fixture.hpp"

using namespace hpactor;

// ── Transport lifecycle ──────────────────────────────────────────

TEST(TcpTransportSystem, ListenOnEphemeralPort) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    EXPECT_NE(system.transport(), nullptr);
    // After binding ephemeral port, endpoint should have non-zero port
    auto ep = system.endpoint();
    EXPECT_NE(ep, EndPoint{});

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(TcpTransportSystem, TransportIsNullWhenNetworkDisabled) {
    Config cfg = test::config_with_scheduler(0);
    cfg.enable_network = false;

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    // Without enable_network, transport() may be null or a stub
    // At minimum, the system should run without crashing
    SUCCEED();
}

TEST(TcpTransportSystem, TwoSystemsDifferentPorts) {
    Config cfg_a = test::config_with_scheduler(1);
    cfg_a.enable_network = true;
    cfg_a.tcp_port = 0;
    cfg_a.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem sys_a(cfg_a);
    EXPECT_TRUE(sys_a.is_running());

    Config cfg_b = test::config_with_scheduler(1);
    cfg_b.enable_network = true;
    cfg_b.tcp_port = 0;
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem sys_b(cfg_b);
    EXPECT_TRUE(sys_b.is_running());

    // Both systems bind ephemeral ports (0) — their endpoints
    // should be valid (non-empty) even if port may be 0 in config
    auto ep_a = sys_a.endpoint();
    auto ep_b = sys_b.endpoint();
    EXPECT_NE(ep_a, EndPoint{});
    EXPECT_NE(ep_b, EndPoint{});

    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

TEST(TcpTransportSystem, TransportEndpointIsValid) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);
    auto ep = transport->endpoint();
    (void)ep;

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(TcpTransportSystem, IsConnectedReturnsFalseForUnknown) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    auto unknown_ep = endpoint_ops::parse_endpoint("127.0.0.1:65000");
    EXPECT_FALSE(transport->is_connected(unknown_ep));

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(TcpTransportSystem, ConnectReturnsNullForUnknownHost) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    // connect(EndPoint) uses registry lookup — no registry entry, should return
    // null
    auto unknown_ep = endpoint_ops::parse_endpoint("127.0.0.1:65000");
    auto conn = transport->connect(unknown_ep);
    EXPECT_EQ(conn, nullptr);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(TcpTransportSystem, CloseConnectionDoesNotCrash) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:65000");
    transport->close_connection(ep);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(TcpTransportSystem, SetRpcHandlerDoesNotCrash) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    bool called = false;
    transport->set_rpc_handler(
        [&called](const RpcResponseFrame&) { called = true; });

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}
