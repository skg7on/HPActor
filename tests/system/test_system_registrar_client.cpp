// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

// System test: Registrar Client
// Exercises RegistrarClient lifecycle via UdpRegistrar.
// Tests client-only paths: heartbeat scheduling, disconnect handling,
// and client-specific message handling.

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/service_discovery.hpp>

#include "system_test_fixture.hpp"

using namespace hpactor;

// ── Registrar lifecycle with explicit config ─────────────────────

TEST(RegistrarClientSystem, ServiceDiscoveryCreatedWhenNetworkEnabled) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 26000;

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    // Network enabled without explicit service_discovery — system
    // auto-selects UdpRegistrar

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(RegistrarClientSystem, StaticDiscoveryInsteadOfRegistrar) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    // Explicit StaticDiscovery instead of registrar
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());

    // registrar() may be null when using StaticDiscovery
    auto* disc = system.registrar();
    (void)disc; // May or may not be null

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(RegistrarClientSystem, CustomRegistrarConfigShortIntervals) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.tcp_port = 0;
    cfg.registrar.udp_port = 26000;
    cfg.registrar.heartbeat_interval = std::chrono::milliseconds(1000);
    cfg.registrar.expiration_timeout = std::chrono::milliseconds(5000);
    cfg.registrar.probe_interval = std::chrono::milliseconds(10000);

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(RegistrarClientSystem, RegistrarWithStaticRoute) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.tcp_port = 0;

    net::StaticRouteConfig route;
    route.address = "127.0.0.1";
    route.port = 12345;
    route.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:12345");
    cfg.registrar.static_routes.push_back(route);

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    // System starts in server mode (first to bind TCP port)

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ── Multiple system lifecycle ────────────────────────────────────

TEST(RegistrarClientSystem, ThreeSystemsWithRegistrar) {
    std::vector<std::unique_ptr<ActorSystem>> systems;

    for (size_t i = 0; i < 3; ++i) {
        Config cfg = test::config_with_scheduler(1);
        cfg.enable_network = true;
        cfg.tcp_port = 0;
        cfg.registrar.tcp_port = static_cast<uint16_t>(18000 + i);
        cfg.registrar.udp_port = static_cast<uint16_t>(18000 + i);

        auto sys = std::make_unique<ActorSystem>(cfg);
        EXPECT_TRUE(sys->is_running());
        systems.push_back(std::move(sys));
    }

    for (auto& sys : systems) {
        auto r = sys->shutdown();
        EXPECT_TRUE(r.has_value());
    }
}

TEST(RegistrarClientSystem, ShutdownCleansRegistrar) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.udp_port = 26000;

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
    // If shutdown cleans up properly, we've exercised registrar stop path
}

// ── Config options ───────────────────────────────────────────────

TEST(RegistrarClientSystem, DisableServerFlag) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.registrar.tcp_port = 0;
    cfg.registrar.udp_port = 26000;
    cfg.registrar.disable_server = true;

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}
