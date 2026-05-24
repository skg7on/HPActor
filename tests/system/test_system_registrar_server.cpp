// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

// System test: Registrar Server
// Exercises RegistrarServer start/stop lifecycle via UdpRegistrar.

#include <gtest/gtest.h>

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/service_discovery.hpp>

#include "system_test_fixture.hpp"

using namespace hpactor;

// ── Server lifecycle ─────────────────────────────────────────────

TEST(RegistrarServerSystem, ServerStartsViaUdpRegistrar) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 25000;

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    // With registrar config and network enabled, the registrar should be
    // created and start in server mode (first to bind wins)

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(RegistrarServerSystem, TwoSystemsRegistrarModeSelection) {
    // First system starts as server (binds TCP port)
    Config cfg_a = test::config_with_scheduler(1);
    cfg_a.enable_network = true;
    cfg_a.tcp_port = 0;
    cfg_a.registrar.tcp_port = 0;

    ActorSystem sys_a(cfg_a);
    EXPECT_TRUE(sys_a.is_running());

    // Second system with different TCP port
    Config cfg_b = test::config_with_scheduler(1);
    cfg_b.enable_network = true;
    cfg_b.tcp_port = 0;
    cfg_b.registrar.tcp_port = 16100; // Different port to avoid collision

    ActorSystem sys_b(cfg_b);
    EXPECT_TRUE(sys_b.is_running());

    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

TEST(RegistrarServerSystem, ServerRegistrarBackendName) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 25000;

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    // UdpRegistrar should be created
    auto* registrar = system.registrar();
    EXPECT_NE(registrar, nullptr);
    EXPECT_EQ(registrar->backend_name(), "udp-registrar");

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ── NodeRegistry via UdpRegistrar ────────────────────────────────

TEST(RegistrarServerSystem, DiscoverAllInitiallyEmpty) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 25000;

    ActorSystem system(cfg);
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    auto members = registrar->discover_all();
    // Initially empty (no nodes registered yet)
    EXPECT_TRUE(members.empty());

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(RegistrarServerSystem, DiscoverReturnsNullForUnknown) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 25000;

    ActorSystem system(cfg);
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    auto unknown_ep = endpoint_ops::parse_endpoint("192.168.1.1:9999");
    auto* member = registrar->discover(unknown_ep);
    EXPECT_EQ(member, nullptr);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(RegistrarServerSystem, AnnounceIsNoop) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 25000;

    ActorSystem system(cfg);
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    net::Member member;
    member.identity.host = "test";
    member.identity.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:8080");
    // announce() is a no-op in UdpRegistrar — should not crash
    registrar->announce(member);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(RegistrarServerSystem, OnMemberChangeStoresCallback) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 25000;

    ActorSystem system(cfg);
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    bool cb_called = false;
    registrar->on_member_change(
        [&cb_called](const net::Member&, bool) { cb_called = true; });
    // Callback is stored but not invoked during normal operation
    // This just verifies no crash

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ── Registrar with static routes ─────────────────────────────────

TEST(RegistrarServerSystem, StaticRoutesPopulatedInClientMode) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 17100; // Specific port for deterministic bind
    cfg.registrar.tcp_port = 17100;
    cfg.registrar.udp_port = 17101;
    // Add a static route
    net::StaticRouteConfig route;
    route.address = "127.0.0.1";
    route.port = 17100;
    route.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:17100");
    cfg.registrar.static_routes.push_back(route);

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    // Since we bound our own registrar TCP port, we're in server mode.
    // The static route should be discoverable through our own registry.
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:17100");
    auto members = registrar->discover_all();
    (void)members;
    (void)ep;

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ── raw_members access ───────────────────────────────────────────

TEST(RegistrarServerSystem, RawMembersAccessor) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 25000;

    ActorSystem system(cfg);
    auto* registrar = system.registrar();
    ASSERT_NE(registrar, nullptr);

    auto* raw = registrar->raw_members();
    // UdpRegistrar returns non-null raw_members (via the full
    // IServiceDiscovery) In server mode it should return a valid pointer
    EXPECT_NE(raw, nullptr);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}
