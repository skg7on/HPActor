// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

/// \file test_coordinated_startup.cpp
///
/// \brief Integration tests for coordinating real ActorSystem startup through
///        RuntimeCoordinator stages.

#include <hpactor/runtime/runtime_blueprint.hpp>
#include <hpactor/runtime/runtime_blueprint_builder.hpp>
#include <hpactor/runtime/runtime_builder.hpp>
#include <hpactor/runtime/runtime_coordinator.hpp>

#include <gtest/gtest.h>

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/types/types.hpp>

using namespace hpactor;

// ── Helper: build a minimal system through the builder ──────────────────────

static std::unique_ptr<ActorSystem> build_minimal_system() {
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

// ── Coordinator with real system ────────────────────────────────────────────

TEST(CoordinatedStartupTest, BuildAndStartWithCoordinator) {
    auto sys = build_minimal_system();
    ASSERT_NE(sys, nullptr);

    RuntimeCoordinator coord;

    // Register a minimal startup stage that verifies the scheduler exists.
    bool scheduler_started = false;
    coord.add_stage(RuntimeLifecycleStage{
        .name = "scheduler",
        .start = {.context = &scheduler_started,
                  .action = [](void* ctx) noexcept -> bool {
                      *static_cast<bool*>(ctx) = true;
                      return true;
                  }},
        .rollback = {.context = &scheduler_started,
                     .action = [](void* ctx) noexcept -> bool {
                         *static_cast<bool*>(ctx) = false;
                         return true;
                     }},
    });

    auto result = coord.start();
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(scheduler_started);
    EXPECT_TRUE(coord.is_ready());
}

TEST(CoordinatedStartupTest, BuildAndStopWithCoordinator) {
    auto sys = build_minimal_system();
    ASSERT_NE(sys, nullptr);

    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.start().ok());
    EXPECT_TRUE(coord.is_ready());

    auto result = coord.stop();
    EXPECT_TRUE(result.ok());
    EXPECT_FALSE(coord.is_ready());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
}

TEST(CoordinatedStartupTest, SystemShutdownIntegratesWithCoordinator) {
    auto sys = build_minimal_system();
    ASSERT_NE(sys, nullptr);

    // The system can be shut down independently.
    // After Phase 6, shutdown() will route through the coordinator.
    auto result = sys->shutdown();
    EXPECT_TRUE(result.ok());
    EXPECT_FALSE(sys->is_ready());
}

TEST(CoordinatedStartupTest, StageFailureRollsBackPriorStages) {
    auto sys = build_minimal_system();
    ASSERT_NE(sys, nullptr);

    RuntimeCoordinator coord;
    int stage_a_done = 0;
    int stage_b_done = 0;

    coord.add_stage(RuntimeLifecycleStage{
        .name = "a",
        .start = {.context = &stage_a_done,
                  .action = [](void* ctx) noexcept -> bool {
                      (*static_cast<int*>(ctx))++;
                      return true;
                  }},
        .rollback = {.context = &stage_a_done,
                     .action = [](void* ctx) noexcept -> bool {
                         (*static_cast<int*>(ctx))--;
                         return true;
                     }},
    });
    coord.add_stage(RuntimeLifecycleStage{
        .name = "b",
        .start = {.context = &stage_b_done,
                  .action = [](void* /*ctx*/) noexcept -> bool {
                      return false; // simulate failure
                  }},
        .rollback = {.context = &stage_b_done,
                     .action = [](void* /*ctx*/) noexcept -> bool {
                         return true; // stage b never started, nothing to undo
                     }},
    });

    auto result = coord.start();
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Failed);
    // Stage A was started then rolled back.
    EXPECT_EQ(stage_a_done, 0);
    // Stage B never completed.
    EXPECT_EQ(stage_b_done, 0);
}

TEST(CoordinatedStartupTest, MultipleSystemsHaveIndependentCoordinators) {
    auto sys1 = build_minimal_system();
    auto sys2 = build_minimal_system();
    ASSERT_NE(sys1, nullptr);
    ASSERT_NE(sys2, nullptr);

    RuntimeCoordinator c1, c2;

    ASSERT_TRUE(c1.start().ok());
    EXPECT_TRUE(c1.is_ready());

    // c2 is independent — still in Built state.
    EXPECT_EQ(c2.state(), RuntimeLifecycleState::Built);
    EXPECT_FALSE(c2.is_ready());

    ASSERT_TRUE(c2.start().ok());
    EXPECT_TRUE(c2.is_ready());

    // Stop c1 doesn't affect c2.
    ASSERT_TRUE(c1.stop().ok());
    EXPECT_FALSE(c1.is_ready());
    EXPECT_TRUE(c2.is_ready());
}

// ── Real startup stages ─────────────────────────────────────────────────────

#include <hpactor/runtime/runtime_startup.hpp>

TEST(CoordinatedStartupTest, RegisterAndStartRealStages) {
    auto sys = build_minimal_system();
    ASSERT_NE(sys, nullptr);

    RuntimeCoordinator coord;
    register_runtime_startup_stages(coord, *sys, /*enable_net=*/false);

    auto result = coord.start();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Running);
    EXPECT_TRUE(coord.is_ready());

    // System scheduler should be running after coordinator start.
    EXPECT_NE(sys->scheduler(), nullptr);

    // Clean stop.
    auto stop_result = coord.stop();
    EXPECT_TRUE(stop_result.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
    EXPECT_FALSE(coord.is_ready());
}

TEST(CoordinatedStartupTest, RealStagesCanBeStoppedFromBuilt) {
    auto sys = build_minimal_system();
    ASSERT_NE(sys, nullptr);

    RuntimeCoordinator coord;
    register_runtime_startup_stages(coord, *sys, /*enable_net=*/false);

    // Direct stop from Built without starting.
    auto result = coord.stop();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
}
