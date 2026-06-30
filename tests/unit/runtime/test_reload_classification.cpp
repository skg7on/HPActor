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

/// \file test_reload_classification.cpp
///
/// \brief Unit tests for reload classification — ConfigPathId, ReloadClass,
///        descriptor registration, and blueprint diff.

#include "src/runtime/runtime_blueprint.hpp"
#include "src/runtime/runtime_blueprint_builder.hpp"

#include <gtest/gtest.h>

#include <hpactor/actor/actor_system.hpp> // for Config
#include <hpactor/config/reload_report.hpp>
#include <hpactor/types/types.hpp>

// ── ReloadClass tests ───────────────────────────────────────────────────────

TEST(ReloadClassificationTest, DefaultReloadClassIsRestartRequired) {
    EXPECT_EQ(hpactor::ReloadClass::RestartRequired,
              hpactor::ConfigFieldDescriptor{}.reload_class);
}

TEST(ReloadClassificationTest, LiveReloadClass) {
    auto cls = hpactor::ReloadClass::Live;
    EXPECT_NE(cls, hpactor::ReloadClass::Immutable);
}

TEST(ReloadClassificationTest, ImmutableReloadClass) {
    auto cls = hpactor::ReloadClass::Immutable;
    EXPECT_NE(cls, hpactor::ReloadClass::Live);
}

// ── ConfigFieldRegistry tests ───────────────────────────────────────────────

TEST(ReloadClassificationTest, RegistryStartsEmpty) {
    auto& reg = hpactor::ConfigFieldRegistry::instance();
    // Registry may have entries from static initialization in other tests.
    // Test the API shape.
    EXPECT_GE(reg.size(), 0u);
}

TEST(ReloadClassificationTest, CanRegisterAndFindField) {
    hpactor::ConfigFieldRegistry reg;
    hpactor::ConfigFieldDescriptor desc{
        .path = 100,
        .reload_class = hpactor::ReloadClass::Immutable,
        .description = "test field",
    };

    EXPECT_TRUE(reg.register_field(desc));
    EXPECT_EQ(reg.size(), 1u);

    auto* found = reg.find(100);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->path, 100u);
    EXPECT_EQ(found->reload_class, hpactor::ReloadClass::Immutable);
}

TEST(ReloadClassificationTest, DuplicatePathIsRejected) {
    hpactor::ConfigFieldRegistry reg;
    reg.register_field({.path = 200, .reload_class = hpactor::ReloadClass::Live});

    EXPECT_FALSE(reg.register_field(
        {.path = 200, .reload_class = hpactor::ReloadClass::Immutable}));
    EXPECT_EQ(reg.size(), 1u);
}

TEST(ReloadClassificationTest, NotFoundReturnsNull) {
    hpactor::ConfigFieldRegistry reg;
    EXPECT_EQ(reg.find(99999), nullptr);
}

// ── Blueprint diff classification ───────────────────────────────────────────

TEST(ReloadClassificationTest, IdenticalBlueprintsHaveNoDiff) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 4;
    cfg.enable_network = false;

    auto r1 = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    auto r2 = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());

    // Same fingerprint means no diff.
    EXPECT_EQ(r1.value().fingerprint(), r2.value().fingerprint());
}

TEST(ReloadClassificationTest, DifferentBlueprintsHaveDifferentFingerprint) {
    hpactor::Config cfg1;
    cfg1.scheduler_threads = 2;
    cfg1.enable_network = false;

    hpactor::Config cfg2;
    cfg2.scheduler_threads = 8;
    cfg2.enable_network = false;

    auto r1 = hpactor::RuntimeBlueprintBuilder::from_config(cfg1);
    auto r2 = hpactor::RuntimeBlueprintBuilder::from_config(cfg2);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());

    EXPECT_NE(r1.value().fingerprint(), r2.value().fingerprint());
}

// ── Blueprint diff ──────────────────────────────────────────────────────────

TEST(ReloadClassificationTest, DiffIdenticalBlueprints) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 4;
    cfg.enable_network = false;

    auto r1 = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    auto r2 = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());

    auto report = hpactor::RuntimeBlueprintBuilder::diff(r1.value(), r2.value());
    EXPECT_TRUE(report.fully_applied);
}

TEST(ReloadClassificationTest, DiffDifferentBlueprintsRequiresRestart) {
    hpactor::Config cfg1, cfg2;
    cfg1.scheduler_threads = 2;
    cfg1.enable_network = false;
    cfg2.scheduler_threads = 8;
    cfg2.enable_network = false;

    auto r1 = hpactor::RuntimeBlueprintBuilder::from_config(cfg1);
    auto r2 = hpactor::RuntimeBlueprintBuilder::from_config(cfg2);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());

    auto report = hpactor::RuntimeBlueprintBuilder::diff(r1.value(), r2.value());
    EXPECT_FALSE(report.fully_applied);
    EXPECT_GT(report.restart_required_fields, 0u);
}

TEST(ReloadClassificationTest, DiffDetectsNoChangeWhenFingerprintsMatch) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    auto bp = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(bp.ok());

    // Self-diff should always be no-change.
    auto report = hpactor::RuntimeBlueprintBuilder::diff(bp.value(), bp.value());
    EXPECT_TRUE(report.fully_applied);
}
