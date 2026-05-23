// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// System test: Graceful Shutdown
// Validates DrainPolicy → lifecycle → DLQ → shutdown coordinator

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>

#include "system_test_fixture.hpp"

#include <gtest/gtest.h>

using namespace hpactor;

using CountingActor = test::CountingActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Shutdown with no user actors completes successfully
// ═══════════════════════════════════════════════════════════════════════════════

TEST(GracefulShutdown, ShutdownNoUserActors) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    EXPECT_TRUE(system.is_running());

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{100};
    opts.actor_drain_timeout = std::chrono::milliseconds{100};
    opts.cluster_leave_timeout = std::chrono::milliseconds{100};
    opts.force_after_timeout = true;

    auto result = system.shutdown(opts);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(system.shutdown_phase(), ShutdownPhase::Stopped);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Shutdown with ImmediateStop actors drains them
// ═══════════════════════════════════════════════════════════════════════════════

TEST(GracefulShutdown, ShutdownWithImmediateStop) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* actor = static_cast<test::CountingActor*>(a1.get().get());

    // Transition to kActive so drain can proceed
    if (actor->as_lifecycle()->state() == LifecycleState::kStarting) {
        actor->as_lifecycle()->transition(LifecycleState::kActive);
    }
    actor->as_lifecycle()->set_drain_config(
        DrainConfig{DrainPolicy::ImmediateStop, std::chrono::milliseconds{500}});

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{500};
    opts.actor_drain_timeout = std::chrono::milliseconds{5000};
    opts.cluster_leave_timeout = std::chrono::milliseconds{500};
    opts.force_after_timeout = true;

    auto result = system.shutdown(opts);
    EXPECT_TRUE(result.has_value());

    // Actor should be stopped
    LifecycleState s = actor->as_lifecycle()->state();
    EXPECT_TRUE(s == LifecycleState::kStopped || s == LifecycleState::kStopping);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Shutdown phase transitions are observable
// ═══════════════════════════════════════════════════════════════════════════════

TEST(GracefulShutdown, ShutdownPhaseTransitions) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    EXPECT_EQ(system.shutdown_phase(), ShutdownPhase::Running);
    EXPECT_TRUE(system.is_ready());

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{100};
    opts.actor_drain_timeout = std::chrono::milliseconds{100};
    opts.cluster_leave_timeout = std::chrono::milliseconds{100};
    opts.force_after_timeout = true;

    // shutdown() sets DrainingIngress first
    auto result = system.shutdown(opts);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(system.shutdown_phase(), ShutdownPhase::Stopped);
    EXPECT_TRUE(!system.is_ready());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Default shutdown (no args) succeeds
// ═══════════════════════════════════════════════════════════════════════════════

TEST(GracefulShutdown, DefaultShutdownSucceeds) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: Drain config can be set per-actor
// ═══════════════════════════════════════════════════════════════════════════════

TEST(GracefulShutdown, PerActorDrainConfig) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* actor = static_cast<test::CountingActor*>(a1.get().get());

    DrainConfig dc{DrainPolicy::DropUserMessages, std::chrono::milliseconds{10000}};
    system.set_drain_config(a1.id(), dc);

    auto configured = actor->as_lifecycle()->drain_config();
    EXPECT_EQ(configured.policy, DrainPolicy::DropUserMessages);
    EXPECT_EQ(configured.timeout, std::chrono::milliseconds{10000});
}
