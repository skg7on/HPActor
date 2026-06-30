// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// \file test_runtime_blueprint.cpp
///
/// \brief Unit tests for RuntimeBlueprint — immutable validated startup input.

#include <hpactor/types/types.hpp>

// Private runtime header — accessible via CMAKE_SOURCE_DIR include path.
#include "src/runtime/runtime_blueprint.hpp"

#include <gtest/gtest.h>

// ── Construction ────────────────────────────────────────────────────────────

TEST(RuntimeBlueprintTest, DefaultConstruction) {
    hpactor::RuntimeBlueprint bp;
    EXPECT_EQ(bp.fingerprint(), 0u);
}

// ── Fingerprint determinism ─────────────────────────────────────────────────

TEST(RuntimeBlueprintTest, SameInputProducesSameFingerprint) {
    hpactor::RuntimeBlueprint bp1;
    // Once we have a builder, same config should produce same fingerprint.
    // For now, verify the accessor works.
    uint64_t fp1 = bp1.fingerprint();
    uint64_t fp2 = bp1.fingerprint();
    EXPECT_EQ(fp1, fp2);
}

// ── Immutability ────────────────────────────────────────────────────────────

TEST(RuntimeBlueprintTest, IsImmutableAfterConstruction) {
    // RuntimeBlueprint has no public setters — it's immutable by design.
    // This test verifies the interface contract.
    const hpactor::RuntimeBlueprint bp;
    EXPECT_EQ(bp.fingerprint(), 0u);

    // Verify const access compiles (documentation test).
    static_assert(std::is_copy_constructible_v<hpactor::RuntimeBlueprint>);
    static_assert(std::is_move_constructible_v<hpactor::RuntimeBlueprint>);
}

// ── Builder: from_config ────────────────────────────────────────────────────

#include "src/runtime/runtime_blueprint_builder.hpp"
#include <hpactor/actor/actor_system.hpp> // for Config

TEST(RuntimeBlueprintBuilderTest, FromConfigProducesNonZeroFingerprint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 4;
    cfg.enable_network = false;

    auto result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(result.ok());

    const auto& bp = result.value();
    // A config with non-default values should produce a non-zero fingerprint.
    EXPECT_NE(bp.fingerprint(), 0u);
}

TEST(RuntimeBlueprintBuilderTest, FromConfigPreservesSchedulerThreads) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 8;
    cfg.enable_network = false;

    auto result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(result.ok());

    const auto& bp = result.value();
    // Blueprint should expose the validated scheduler thread count.
    EXPECT_EQ(bp.actor().scheduler_threads, 8u);
}

TEST(RuntimeBlueprintBuilderTest, FromConfigPreservesEndpoint) {
    hpactor::Config cfg;
    cfg.endpoint = hpactor::endpoint_ops::parse_endpoint("192.168.1.1:8080");
    cfg.enable_network = false;

    auto result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(result.ok());

    const auto& bp = result.value();
    EXPECT_EQ(bp.actor().endpoint, cfg.endpoint);
}

TEST(RuntimeBlueprintBuilderTest, SameConfigProducesSameFingerprint) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 2;
    cfg.enable_network = false;

    auto r1 = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    auto r2 = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());

    EXPECT_EQ(r1.value().fingerprint(), r2.value().fingerprint());
}

TEST(RuntimeBlueprintBuilderTest, DifferentConfigProducesDifferentFingerprint) {
    hpactor::Config cfg1;
    cfg1.scheduler_threads = 2;
    cfg1.enable_network = false;

    hpactor::Config cfg2;
    cfg2.scheduler_threads = 4;
    cfg2.enable_network = false;

    auto r1 = hpactor::RuntimeBlueprintBuilder::from_config(cfg1);
    auto r2 = hpactor::RuntimeBlueprintBuilder::from_config(cfg2);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());

    EXPECT_NE(r1.value().fingerprint(), r2.value().fingerprint());
}

// ── Builder: validation ──────────────────────────────────────────────────────

TEST(RuntimeBlueprintBuilderTest, AcceptsZeroSchedulerThreads) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0; // valid — no workers
    cfg.enable_network = false;

    auto result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    EXPECT_TRUE(result.ok());
}

TEST(RuntimeBlueprintBuilderTest, RejectsNetworkEnabledWithoutPort) {
    hpactor::Config cfg;
    cfg.enable_network = true;
    cfg.tcp_port = 0; // invalid — must specify port when network enabled

    auto result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    EXPECT_TRUE(result.is_error());
}
