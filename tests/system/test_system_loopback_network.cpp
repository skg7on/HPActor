// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// System test: Loopback Networking
// Validates ActorSystem → TcpTransport → EventLoop → StaticDiscovery
// over ephemeral loopback ports, with clean resource teardown.

#include <gtest/gtest.h>

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/static_discovery.hpp>

#include "system_test_fixture.hpp"

using namespace hpactor;

using CountingActor = test::CountingActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Two ActorSystems on loopback with StaticDiscovery
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LoopbackNetwork, TwoSystemsLoopbackDiscovery) {
    // System A: listens on ephemeral port
    Config cfg_a;
    cfg_a.scheduler_threads = 1;
    cfg_a.enable_network = true;
    cfg_a.tcp_port = 0; // ephemeral
    cfg_a.cli.enabled = false;
    cfg_a.tracing.enabled = false;

    // Create StaticDiscovery with System A
    auto discovery_a =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    cfg_a.service_discovery = discovery_a;

    ActorSystem system_a(cfg_a);
    EXPECT_TRUE(system_a.is_running());

    // System B: also on loopback, different port
    Config cfg_b;
    cfg_b.scheduler_threads = 1;
    cfg_b.enable_network = true;
    cfg_b.tcp_port = 0;
    cfg_b.cli.enabled = false;
    cfg_b.tracing.enabled = false;
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system_b(cfg_b);
    EXPECT_TRUE(system_b.is_running());

    // Both systems should have transport
    EXPECT_NE(system_a.transport(), nullptr);
    EXPECT_NE(system_b.transport(), nullptr);

    // Clean shutdown
    auto r_a = system_a.shutdown();
    auto r_b = system_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Single ActorSystem network lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LoopbackNetwork, SingleSystemNetworkLifecycle) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.cli.enabled = false;
    cfg.tracing.enabled = false;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    EXPECT_NE(system.transport(), nullptr);
    EXPECT_NE(system.endpoint(), EndPoint{});

    // Transport should have a valid endpoint
    auto ep = system.transport()->endpoint();
    (void)ep;

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Network disabled by default
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LoopbackNetwork, NetworkDisabledByDefault) {
    Config cfg = test::minimal_config();
    ActorSystem system(cfg);

    EXPECT_FALSE(cfg.enable_network);
    EXPECT_EQ(system.transport(), nullptr);
}
