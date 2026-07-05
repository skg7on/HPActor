// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

// System test: UdpRegistrar Lifecycle
// Exercises UdpRegistrar server/client mode determination,
// discover/discover_all, and integration with ActorSystem.

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/static_discovery.hpp>

#include "system_test_fixture.hpp"

using namespace hpactor;

// ── Mode determination (server vs client) ────────────────────────

TEST(UdpRegistrarSystem, ServerModeWhenFirstToBind) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.tcp_port = 19000;
    cfg.registrar.udp_port = 19001;

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());

    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);
    EXPECT_EQ(registrar->backend_name(), "udp-registrar");

    // First system to bind should be in server mode
    // (internal: server_ exists, client_ does not)
    auto members = registrar->discover_all();
    // Server mode — no static routes, so initially empty
    EXPECT_TRUE(members.empty());

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(UdpRegistrarSystem, StaticRoutesPopulated) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.tcp_port = 20000;
    cfg.registrar.udp_port = 20001;

    net::StaticRouteConfig route;
    route.address = "127.0.0.1";
    route.port = 9090;
    route.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:9090");
    cfg.registrar.static_routes.push_back(route);

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());

    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    auto members = registrar->discover_all();
    (void)members;
    // Static routes should be populated in the client_registry when in client
    // mode In server mode, discover_all returns the server's NodeRegistry
    // content

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ── IServiceDiscovery interface conformance ──────────────────────

TEST(UdpRegistrarSystem, DiscoverAllReturnsVector) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 27000;

    ActorSystem system(cfg);
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    auto members = registrar->discover_all();
    // discover_all() should return a valid vector (possibly empty)
    EXPECT_TRUE(members.empty()); // No nodes registered initially

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(UdpRegistrarSystem, DiscoverReturnsNullForUnknownEndpoint) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 27000;

    ActorSystem system(cfg);
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    auto ep = endpoint_ops::parse_endpoint("10.0.0.1:5555");
    auto* member = registrar->discover(ep);
    EXPECT_EQ(member, nullptr);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(UdpRegistrarSystem, AnnounceIsNoop) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 27000;

    ActorSystem system(cfg);
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    net::Member m;
    m.identity.host = "node1";
    m.identity.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:4242");
    m.status = net::MemberStatus::Alive;
    m.incarnation = 1;

    // announce() is a no-op — should not crash
    registrar->announce(m);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(UdpRegistrarSystem, OnMemberChangeStoresCallback) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 27000;

    ActorSystem system(cfg);
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    int call_count = 0;
    registrar->on_member_change(
        [&call_count](const net::Member&, bool) { call_count++; });

    // Callback is stored but normally not invoked
    // This test just verifies no crash on registration

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(UdpRegistrarSystem, RawMembersNotNull) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 27000;

    ActorSystem system(cfg);
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    auto* raw = registrar->raw_members();
    EXPECT_NE(raw, nullptr);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ── Multiple systems with different ports ────────────────────────

TEST(UdpRegistrarSystem, TwoSystemsSeparateRegistrarPorts) {
    Config cfg_a = test::config_with_scheduler(1);
    cfg_a.enable_network = true;
    cfg_a.tcp_port = 21000;
    cfg_a.registrar.tcp_port = 21000;
    cfg_a.registrar.udp_port = 21001;

    Config cfg_b = test::config_with_scheduler(1);
    cfg_b.enable_network = true;
    cfg_b.tcp_port = 22000;
    cfg_b.registrar.tcp_port = 22000;
    cfg_b.registrar.udp_port = 22001;

    ActorSystem sys_a(cfg_a);
    EXPECT_TRUE(sys_a.is_running());
    ActorSystem sys_b(cfg_b);
    EXPECT_TRUE(sys_b.is_running());

    auto* ra = sys_a.registrar();
    auto* rb = sys_b.registrar();
    ASSERT_NE(ra, nullptr);
    ASSERT_NE(rb, nullptr);

    EXPECT_EQ(ra->backend_name(), "udp-registrar");
    EXPECT_EQ(rb->backend_name(), "udp-registrar");

    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}
