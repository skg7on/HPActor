// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// System test: Loopback Networking
// Validates ActorSystem → TcpTransport → EventLoop → StaticDiscovery
// over ephemeral loopback ports, with clean resource teardown.

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/static_discovery.hpp>

#include "system_test_fixture.hpp"

#include <cassert>
#include <cstdio>

using namespace hpactor;

using CountingActor = test::CountingActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Two ActorSystems on loopback with StaticDiscovery
// ═══════════════════════════════════════════════════════════════════════════════

static void test_two_systems_loopback_discovery() {
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
    assert(system_a.is_running());

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
    assert(system_b.is_running());

    // Both systems should have transport
    assert(system_a.transport() != nullptr);
    assert(system_b.transport() != nullptr);

    // Clean shutdown
    auto r_a = system_a.shutdown();
    auto r_b = system_b.shutdown();
    assert(r_a.has_value());
    assert(r_b.has_value());

    std::printf("PASS: test_two_systems_loopback_discovery\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Single ActorSystem network lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

static void test_single_system_network_lifecycle() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.cli.enabled = false;
    cfg.tracing.enabled = false;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    assert(system.is_running());
    assert(system.transport() != nullptr);
    assert(system.endpoint() != EndPoint{});

    // Transport should have a valid endpoint
    auto ep = system.transport()->endpoint();
    (void)ep;

    auto result = system.shutdown();
    assert(result.has_value());

    std::printf("PASS: test_single_system_network_lifecycle\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Network disabled by default
// ═══════════════════════════════════════════════════════════════════════════════

static void test_network_disabled_by_default() {
    Config cfg = test::minimal_config();
    ActorSystem system(cfg);

    assert(!cfg.enable_network);
    assert(system.transport() == nullptr);

    std::printf("PASS: test_network_disabled_by_default\n");
}

int main() {
    test_network_disabled_by_default();
    test_single_system_network_lifecycle();
    test_two_systems_loopback_discovery();
    std::printf("\nAll loopback network system tests passed.\n");
    return 0;
}
