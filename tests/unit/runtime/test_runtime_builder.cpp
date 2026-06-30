// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

/// \file test_runtime_builder.cpp
///
/// \brief Unit tests for RuntimeBuilder — constructs stopped component graph
///        from an immutable RuntimeBlueprint without starting anything.

#include "src/runtime/runtime_blueprint.hpp"
#include "src/runtime/runtime_blueprint_builder.hpp"
#include "src/runtime/runtime_builder.hpp"

#include <gtest/gtest.h>

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/types/types.hpp>

// ── Construction from blueprint ──────────────────────────────────────────────

TEST(RuntimeBuilderTest, BuildFromBlueprintWithoutNetwork) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    auto bp_result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(bp_result.ok());

    auto build_result = hpactor::RuntimeBuilder::build(bp_result.value());
    ASSERT_TRUE(build_result.ok());
    EXPECT_NE(build_result.value().system, nullptr);
}

TEST(RuntimeBuilderTest, BuildProducesValidSystem) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    auto bp_result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(bp_result.ok());

    auto build_result = hpactor::RuntimeBuilder::build(bp_result.value());
    ASSERT_TRUE(build_result.ok());

    auto& sys = *build_result.value().system;
    // System is valid but not ready (no startup performed).
    EXPECT_NE(sys.scheduler(), nullptr);
}

TEST(RuntimeBuilderTest, BuildDoesNotStartThreads) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    auto bp_result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(bp_result.ok());

    auto build_result = hpactor::RuntimeBuilder::build(bp_result.value());
    ASSERT_TRUE(build_result.ok());
    // No threads, no listeners, no actors created.
    SUCCEED();
}

TEST(RuntimeBuilderTest, BuildWithZeroThreads) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    auto bp_result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(bp_result.ok());

    auto build_result = hpactor::RuntimeBuilder::build(bp_result.value());
    ASSERT_TRUE(build_result.ok());
}

TEST(RuntimeBuilderTest, GraphIsDestroyedCleanly) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    auto bp_result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(bp_result.ok());

    {
        auto build_result = hpactor::RuntimeBuilder::build(bp_result.value());
        ASSERT_TRUE(build_result.ok());
    }
    SUCCEED();
}

TEST(RuntimeBuilderTest, RepeatedBuildsProduceIndependentGraphs) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    auto bp_result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(bp_result.ok());

    auto r1 = hpactor::RuntimeBuilder::build(bp_result.value());
    auto r2 = hpactor::RuntimeBuilder::build(bp_result.value());

    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());
    EXPECT_NE(r1.value().system.get(), r2.value().system.get());
}
