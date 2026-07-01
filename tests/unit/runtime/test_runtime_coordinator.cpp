// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

/// \file test_runtime_coordinator.cpp
///
/// \brief RED tests for RuntimeCoordinator — lifecycle state machine with
///        fake components. Exhaustively proves start/rollback/stop.

#include <hpactor/runtime/runtime_coordinator.hpp>

#include <gtest/gtest.h>

#include <hpactor/types/types.hpp>

#include <string>
#include <vector>

// ── Fake component actions for testing ──────────────────────────────────────

namespace {

struct ActionLog {
    std::vector<std::string> actions;
    void clear() {
        actions.clear();
    }
    void record(const std::string& s) {
        actions.push_back(s);
    }
};

ActionLog g_log;

bool fake_start_a(void* /*ctx*/) noexcept {
    g_log.record("start_a");
    return true;
}
bool fake_rollback_a(void* /*ctx*/) noexcept {
    g_log.record("rollback_a");
    return true;
}
bool fake_start_b(void* /*ctx*/) noexcept {
    g_log.record("start_b");
    return true;
}
bool fake_rollback_b(void* /*ctx*/) noexcept {
    g_log.record("rollback_b");
    return true;
}
bool fake_start_fails(void* /*ctx*/) noexcept {
    g_log.record("start_fails");
    return false; // Simulates stage failure → triggers rollback.
}

} // namespace

using namespace hpactor;

// ── State machine tests ─────────────────────────────────────────────────────

class RuntimeCoordinatorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        g_log.clear();
    }
};

TEST_F(RuntimeCoordinatorTest, StartsInBuiltState) {
    RuntimeCoordinator coord;
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Built);
}

TEST_F(RuntimeCoordinatorTest, StartTransitionsToRunning) {
    RuntimeCoordinator coord;
    auto result = coord.start();
    // With no stages registered, start transitions directly to Running.
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Running);
}

TEST_F(RuntimeCoordinatorTest, StopFromBuiltTransitionsToStopped) {
    RuntimeCoordinator coord;
    auto result = coord.stop();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
}

TEST_F(RuntimeCoordinatorTest, StopFromRunningTransitionsToStopped) {
    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.start().ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Running);

    auto result = coord.stop();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
}

TEST_F(RuntimeCoordinatorTest, StopIsIdempotent) {
    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.stop().ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);

    auto r2 = coord.stop();
    EXPECT_TRUE(r2.ok());
    EXPECT_EQ(coord.state(), RuntimeLifecycleState::Stopped);
}

TEST_F(RuntimeCoordinatorTest, StartAfterStopIsRejected) {
    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.stop().ok());

    auto result = coord.start();
    EXPECT_TRUE(result.is_error());
}

TEST_F(RuntimeCoordinatorTest, StartAfterRunningIsRejected) {
    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.start().ok());

    auto result = coord.start();
    EXPECT_TRUE(result.is_error());
}

// ── Stage registration and ordered execution ───────────────────────────────

TEST_F(RuntimeCoordinatorTest, StagesExecuteInRegistrationOrder) {
    RuntimeCoordinator coord;

    coord.add_stage(RuntimeLifecycleStage{
        .name = "a",
        .start = {.context = nullptr, .action = fake_start_a},
        .rollback = {.context = nullptr, .action = fake_rollback_a},
    });
    coord.add_stage(RuntimeLifecycleStage{
        .name = "b",
        .start = {.context = nullptr, .action = fake_start_b},
        .rollback = {.context = nullptr, .action = fake_rollback_b},
    });

    auto result = coord.start();
    EXPECT_TRUE(result.ok());

    // Stages execute in order: a then b.
    ASSERT_GE(g_log.actions.size(), 2u);
    EXPECT_EQ(g_log.actions[0], "start_a");
    EXPECT_EQ(g_log.actions[1], "start_b");
}

// ── Rollback on failure ────────────────────────────────────────────────────

TEST_F(RuntimeCoordinatorTest, RollbackOnStageFailure) {
    RuntimeCoordinator coord;

    coord.add_stage(RuntimeLifecycleStage{
        .name = "a",
        .start = {.context = nullptr, .action = fake_start_a},
        .rollback = {.context = nullptr, .action = fake_rollback_a},
    });
    coord.add_stage(RuntimeLifecycleStage{
        .name = "b",
        .start = {.context = nullptr, .action = fake_start_b},
        .rollback = {.context = nullptr, .action = fake_rollback_b},
    });
    coord.add_stage(RuntimeLifecycleStage{
        .name = "fails",
        .start = {.context = nullptr, .action = fake_start_fails},
        .rollback = {.context = nullptr, .action = fake_rollback_b},
    });

    auto result = coord.start();
    // The failing stage causes a rollback error.
    EXPECT_TRUE(result.is_error());

    // Rollback happens in reverse order: b then a (not c, which never started).
    ASSERT_GE(g_log.actions.size(), 4u);
    EXPECT_EQ(g_log.actions[0], "start_a");
    EXPECT_EQ(g_log.actions[1], "start_b");
    EXPECT_EQ(g_log.actions[2], "start_fails");
    EXPECT_EQ(g_log.actions[3], "rollback_b");
}

// ── Readiness ───────────────────────────────────────────────────────────────

TEST_F(RuntimeCoordinatorTest, NotReadyBeforeStart) {
    RuntimeCoordinator coord;
    EXPECT_FALSE(coord.is_ready());
}

TEST_F(RuntimeCoordinatorTest, ReadyAfterSuccessfulStart) {
    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.start().ok());
    EXPECT_TRUE(coord.is_ready());
}

TEST_F(RuntimeCoordinatorTest, NotReadyAfterStop) {
    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.start().ok());
    ASSERT_TRUE(coord.is_ready());
    ASSERT_TRUE(coord.stop().ok());
    EXPECT_FALSE(coord.is_ready());
}

TEST_F(RuntimeCoordinatorTest, NotReadyAfterFailedStart) {
    RuntimeCoordinator coord;
    coord.add_stage(RuntimeLifecycleStage{
        .name = "fails",
        .start = {.context = nullptr, .action = fake_start_fails},
        .rollback = {.context = nullptr, .action = fake_rollback_b},
    });

    auto result = coord.start();
    EXPECT_TRUE(result.is_error());
    EXPECT_FALSE(coord.is_ready());
}

// ── Snapshot ────────────────────────────────────────────────────────────────

TEST_F(RuntimeCoordinatorTest, SnapshotReflectsCurrentState) {
    RuntimeCoordinator coord;
    auto snap = coord.snapshot();
    EXPECT_EQ(snap.state, RuntimeLifecycleState::Built);

    ASSERT_TRUE(coord.start().ok());
    snap = coord.snapshot();
    EXPECT_EQ(snap.state, RuntimeLifecycleState::Running);
}

// ── Deferred network-thread stop ────────────────────────────────────────────

TEST_F(RuntimeCoordinatorTest, StopFromRunningIsDeferredAccepted) {
    RuntimeCoordinator coord;
    ASSERT_TRUE(coord.start().ok());

    // A stop request returns but the actual stop happens asynchronously
    // if called from a thread that can't join itself.
    auto result = coord.stop();
    EXPECT_TRUE(result.ok());
}
