// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

/// \file test_unified_shutdown.cpp
/// \brief Tests for unified shutdown through RuntimeCoordinator.

#include "src/runtime/runtime_blueprint.hpp"
#include "src/runtime/runtime_blueprint_builder.hpp"
#include "src/runtime/runtime_builder.hpp"
#include "src/runtime/runtime_coordinator.hpp"
#include "src/runtime/runtime_startup.hpp"

#include <gtest/gtest.h>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/types/types.hpp>

using namespace hpactor;

namespace {
std::unique_ptr<ActorSystem> build_minimal_system() {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    auto bp = RuntimeBlueprintBuilder::from_config(cfg);
    if (!bp.ok())
        return nullptr;
    auto built = RuntimeBuilder::build(bp.value());
    if (!built.ok())
        return nullptr;
    return std::move(built.value().system);
}
} // namespace

// ── Unified shutdown path ───────────────────────────────────────────────────

TEST(UnifiedShutdownTest, ShutdownTransitionsThroughDrain) {
    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.start().ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Running);

    auto result = coord.shutdown();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
    EXPECT_FALSE(coord.is_ready());
}

TEST(UnifiedShutdownTest, ShutdownFromBuiltGoesToStopped) {
    RuntimeCoordinator coord;
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Built);

    auto result = coord.shutdown();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
}

TEST(UnifiedShutdownTest, ShutdownIsIdempotent) {
    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.shutdown().ok());
    auto r2 = coord.shutdown();
    EXPECT_TRUE(r2.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
}

TEST(UnifiedShutdownTest, ShutdownAfterFailedStart) {
    RuntimeCoordinator coord;
    coord.add_stage(RuntimeLifecycleStage{
        .name = "fails",
        .start = {.context = nullptr,
                  .action = [](void*) noexcept -> bool { return false; }},
        .rollback = {},
    });
    ASSERT_TRUE(coord.start().is_error());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Failed);

    auto result = coord.shutdown();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
}

TEST(UnifiedShutdownTest, ShutdownAndStopAreEquivalent) {
    RuntimeCoordinator c1, c2;
    ASSERT_TRUE(c1.start().ok());
    ASSERT_TRUE(c2.start().ok());

    c1.shutdown();
    c2.stop();

    EXPECT_EQ(c1.state(), RuntimeLifecycleState::Stopped);
    EXPECT_EQ(c2.state(), RuntimeLifecycleState::Stopped);
}

TEST(UnifiedShutdownTest, ReadinessClearedBeforeDrain) {
    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.start().ok());
    EXPECT_TRUE(coord.is_ready());

    // After shutdown starts, readiness is false.
    (void)coord.shutdown();
    EXPECT_FALSE(coord.is_ready());
}

TEST(UnifiedShutdownTest, DestructorCallsStopImplicitly) {
    // Coordinator destructor frees contexts even if stop() wasn't called.
    {
        RuntimeCoordinator coord;
        ASSERT_TRUE(coord.start().ok());
        // Destructor runs — no crash, no leak.
    }
    SUCCEED();
}

// ── Real system shutdown ────────────────────────────────────────────────────

TEST(UnifiedShutdownTest, RealSystemShutdownThroughCoordinator) {
    auto sys = build_minimal_system();
    ASSERT_NE(sys, nullptr);

    RuntimeCoordinator coord;
    register_runtime_startup_stages(coord, *sys, false);
    ASSERT_TRUE(coord.start().ok());

    auto result = coord.shutdown();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
}

TEST(UnifiedShutdownTest, RepeatedShutdownIsSafe) {
    auto sys = build_minimal_system();
    ASSERT_NE(sys, nullptr);

    for (int i = 0; i < 3; ++i) {
        RuntimeCoordinator coord;
        register_runtime_startup_stages(coord, *sys, false);
        ASSERT_TRUE(coord.start().ok());
        ASSERT_TRUE(coord.shutdown().ok());
    }
    SUCCEED();
}
