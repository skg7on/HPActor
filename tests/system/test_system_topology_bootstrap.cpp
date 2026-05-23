// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// System test: Topology Bootstrap
// Validates TOML → ActorFactoryRegistry → spawn_configured → SystemInit →
// shutdown

#include <gtest/gtest.h>

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>

#include "system_test_fixture.hpp"

#include <string>

using namespace hpactor;

// ── Register test actors for TOML bootstrapping ──────────────────────────────

using CountingActor = test::CountingActor;
using EchoActor = test::EchoActor;

HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);
HPACTOR_REGISTER_ACTOR("EchoActor", EchoActor);

// ── Helper: get test data path ───────────────────────────────────────────────

static std::string data_path(const char* filename) {
#ifdef TEST_DATA_DIR
    return std::string(TEST_DATA_DIR) + "/" + filename;
#else
    return std::string("tests/data/toml/") + filename;
#endif
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Load a 3-actor TOML topology, verify all actors spawned
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TopologyBootstrap, TopologySpawnsAllActors) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto result = system.load_topology(data_path("system_test_topology.toml"));
    EXPECT_TRUE(result.has_value());

    size_t count = system.actor_count();
    EXPECT_GE(count, 3);

    std::vector<ActorId> actor_ids;
    system.for_each_actor(
        [&](ActorId id, AbstractActor& /*actor*/) { actor_ids.push_back(id); });

    for (auto id : actor_ids) {
        auto actor = system.get_actor(id);
        EXPECT_NE(actor, nullptr);
    }

    for (const char* name : {"alpha", "beta", "gamma"}) {
        auto addr = system.registry().get(name);
        EXPECT_NE(addr.id, ActorId(0));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: All topology actors alive after bootstrap
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TopologyBootstrap, AllActorsAliveAfterBootstrap) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto result = system.load_topology(data_path("system_test_topology.toml"));
    EXPECT_TRUE(result.has_value());

    // Poll: at least 3 user actors exist and have lifecycle
    bool all_alive = test::assert_eventually(
        [&]() {
            if (system.actor_count() < 3)
                return false;
            bool ok = true;
            system.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
                if (actor.is_system_actor())
                    return;
                if (!actor.as_lifecycle())
                    ok = false;
            });
            return ok;
        },
        5000);

    EXPECT_TRUE(all_alive);

    // Verify all three named actors are in registry
    auto alpha_addr = system.registry().get("alpha");
    auto beta_addr = system.registry().get("beta");
    auto gamma_addr = system.registry().get("gamma");

    EXPECT_NE(alpha_addr.id, ActorId(0));
    EXPECT_NE(beta_addr.id, ActorId(0));
    EXPECT_NE(gamma_addr.id, ActorId(0));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Clean shutdown after topology boot
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TopologyBootstrap, CleanShutdownAfterTopologyLoad) {
    Config cfg = test::config_with_scheduler(1);
    cfg.shutdown_drain =
        DrainConfig{DrainPolicy::ImmediateStop, std::chrono::milliseconds{500}};
    ActorSystem system(cfg);

    auto load_result = system.load_topology(data_path("system_test_topology."
                                                      "toml"));
    EXPECT_TRUE(load_result.has_value());

    EXPECT_TRUE(system.is_running());
    EXPECT_EQ(system.shutdown_phase(), ShutdownPhase::Running);

    // Transition actors to kActive (spawn_configured leaves them in kStarting)
    // and set ImmediateStop for fast shutdown.
    system.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        if (auto* lc = actor.as_lifecycle()) {
            // kStarting → kActive is the valid path
            if (lc->state() == LifecycleState::kStarting) {
                lc->transition(LifecycleState::kActive);
            }
            lc->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop,
                                             std::chrono::milliseconds{500}});
        }
    });

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{500};
    opts.actor_drain_timeout = std::chrono::milliseconds{2000};
    opts.cluster_leave_timeout = std::chrono::milliseconds{500};
    opts.force_after_timeout = true;

    auto shutdown_result = system.shutdown(opts);
    EXPECT_TRUE(shutdown_result.has_value());

    // After shutdown, all non-system actors should be stopped
    system.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        if (auto* lc = actor.as_lifecycle()) {
            LifecycleState s = lc->state();
            EXPECT_TRUE(s == LifecycleState::kStopped ||
                        s == LifecycleState::kStopping);
        }
    });
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Unknown behavior returns error
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TopologyBootstrap, UnknownBehaviorReturnsError) {
    Config cfg = test::minimal_config();
    ActorSystem system(cfg);

    // supervisor_tree.toml uses "ParentActor" and "ChildActor" — unregistered
    auto result = system.load_topology(data_path("supervisor_tree.toml"));
    EXPECT_FALSE(result.has_value());
}
