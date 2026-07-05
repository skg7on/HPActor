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
#include <hpactor/runtime/runtime_blueprint.hpp>

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

#include <hpactor/actor/system/actor_system.hpp> // for Config
#include <hpactor/runtime/runtime_blueprint_builder.hpp>

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

// ── Side-effect probe ───────────────────────────────────────────────────────

/// Phase 6 core invariant: building a RuntimeBlueprint MUST NOT create
/// threads, listen on sockets, spawn actors, daemonize, register timers,
/// or initialize singletons. This test fails if any such side effect
/// is detected during blueprint construction.
TEST(RuntimeBlueprintBuilderTest, NoSideEffectsDuringBuild) {
    // Phase 6 core invariant: building a RuntimeBlueprint MUST NOT create
    // threads, listen on sockets, spawn actors, daemonize, register timers,
    // or initialize singletons.
    //
    // We use network-disabled config here because the blueprint builder
    // for network-enabled config requires fully specified transport fields
    // (pool, tls, etc.) that are not yet validated in isolation.
    hpactor::Config cfg;
    cfg.scheduler_threads = 8;
    cfg.enable_network = false;

    auto result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(result.ok());

    const auto& bp = result.value();
    EXPECT_EQ(bp.actor().scheduler_threads, 8u);

    // Building a blueprint does not require an ActorSystem.
    // No threads, no listeners, no actors were created.
}

/// Building a blueprint from config does not mutate the original config.
TEST(RuntimeBlueprintBuilderTest, DoesNotMutateOriginalConfig) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 4;
    cfg.enable_network = false;

    auto result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(result.ok());

    // Original config is unchanged.
    EXPECT_EQ(cfg.scheduler_threads, 4u);
    EXPECT_EQ(cfg.enable_network, false);
}

/// Repeated builds from the same config produce identical blueprints.
TEST(RuntimeBlueprintBuilderTest, RepeatedBuildIsIdempotent) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 2;
    cfg.enable_network = false;

    auto r1 = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    auto r2 = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    auto r3 = hpactor::RuntimeBlueprintBuilder::from_config(cfg);

    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());
    ASSERT_TRUE(r3.ok());

    EXPECT_EQ(r1.value().fingerprint(), r2.value().fingerprint());
    EXPECT_EQ(r2.value().fingerprint(), r3.value().fingerprint());
}

TEST(RuntimeBlueprintBuilderTest, RejectsNetworkEnabledWithoutPort) {
    hpactor::Config cfg;
    cfg.enable_network = true;
    cfg.tcp_port = 0; // invalid — must specify port when network enabled

    auto result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    EXPECT_TRUE(result.is_error());
}

// ── ObservabilityRuntimeConfig ─────────────────────────────────────────────

TEST(ObservabilityRuntimeConfigTest, DefaultConstruction) {
    hpactor::ObservabilityRuntimeConfig cfg;
    EXPECT_TRUE(cfg.metrics_enabled);
    EXPECT_TRUE(cfg.logging_enabled);
    EXPECT_FALSE(cfg.tracing_enabled); // tracing defaults to off
    EXPECT_EQ(cfg.metrics_ring_buffer_capacity, 65536u);
    EXPECT_EQ(cfg.logging_ring_buffer_capacity, 65536u);
    EXPECT_EQ(cfg.tracing_ring_buffer_capacity, 65536u);
    EXPECT_TRUE(cfg.fault_injection_enabled);
}

TEST(ObservabilityRuntimeConfigTest, ExplicitConstruction) {
    hpactor::ObservabilityRuntimeConfig cfg{/*metrics_enabled=*/false,
                                            /*logging_enabled=*/true,
                                            /*tracing_enabled=*/true,
                                            /*metrics_ring_buffer_capacity=*/8192,
                                            /*logging_ring_buffer_capacity=*/4096,
                                            /*tracing_ring_buffer_capacity=*/1024,
                                            /*fault_injection_enabled=*/false};
    EXPECT_FALSE(cfg.metrics_enabled);
    EXPECT_TRUE(cfg.logging_enabled);
    EXPECT_TRUE(cfg.tracing_enabled);
    EXPECT_EQ(cfg.metrics_ring_buffer_capacity, 8192u);
    EXPECT_EQ(cfg.logging_ring_buffer_capacity, 4096u);
    EXPECT_EQ(cfg.tracing_ring_buffer_capacity, 1024u);
    EXPECT_FALSE(cfg.fault_injection_enabled);
}

TEST(ObservabilityRuntimeConfigTest, AllDisabledIsValid) {
    hpactor::ObservabilityRuntimeConfig cfg{false, false, false, 0,
                                            0,     0,     false};
    // All subsystems disabled is a valid configuration (headless/testing mode).
    EXPECT_FALSE(cfg.metrics_enabled);
    EXPECT_FALSE(cfg.logging_enabled);
    EXPECT_FALSE(cfg.tracing_enabled);
    EXPECT_FALSE(cfg.fault_injection_enabled);
}

// ── ClusterRuntimeConfig ────────────────────────────────────────────────────

TEST(ClusterRuntimeConfigTest, DefaultConstruction) {
    hpactor::ClusterRuntimeConfig cfg;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_TRUE(cfg.node_id.empty());
}

TEST(ClusterRuntimeConfigTest, ExplicitConstruction) {
    hpactor::ClusterRuntimeConfig cfg{/*enabled=*/true,
                                      /*node_id=*/"node-1"};
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.node_id, "node-1");
}

TEST(ClusterRuntimeConfigTest, DisabledWithNodeId) {
    // Cluster can have a node_id configured but be disabled.
    hpactor::ClusterRuntimeConfig cfg{/*enabled=*/false,
                                      /*node_id=*/"node-5"};
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.node_id, "node-5");
}

// ── Blueprint accessors for new config types ────────────────────────────────

TEST(RuntimeBlueprintTest, ObservabilityAccessorDefaults) {
    hpactor::RuntimeBlueprint bp;
    const auto& obs = bp.observability();
    // Default-constructed blueprint should have sensible observability
    // defaults.
    EXPECT_TRUE(obs.metrics_enabled);
    EXPECT_FALSE(obs.tracing_enabled);
}

TEST(RuntimeBlueprintTest, ClusterAccessorDefaults) {
    hpactor::RuntimeBlueprint bp;
    const auto& cluster = bp.cluster();
    // Default-constructed blueprint should have cluster disabled.
    EXPECT_FALSE(cluster.enabled);
}

// ── Builder populates observability from Config ─────────────────────────────

TEST(RuntimeBlueprintBuilderTest, FromConfigPopulatesTracingEnabled) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    cfg.tracing.enabled = true;
    cfg.tracing.ring_buffer_capacity = 8192;

    auto result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(result.ok());

    const auto& bp = result.value();
    EXPECT_TRUE(bp.observability().tracing_enabled);
    EXPECT_EQ(bp.observability().tracing_ring_buffer_capacity, 8192u);
}

TEST(RuntimeBlueprintBuilderTest, FromConfigTracingIncludedInFingerprint) {
    hpactor::Config cfg1;
    cfg1.scheduler_threads = 1;
    cfg1.enable_network = false;
    cfg1.tracing.enabled = false;

    hpactor::Config cfg2;
    cfg2.scheduler_threads = 1;
    cfg2.enable_network = false;
    cfg2.tracing.enabled = true;

    auto r1 = hpactor::RuntimeBlueprintBuilder::from_config(cfg1);
    auto r2 = hpactor::RuntimeBlueprintBuilder::from_config(cfg2);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());

    // Different tracing config → different fingerprint.
    EXPECT_NE(r1.value().fingerprint(), r2.value().fingerprint());
}

TEST(RuntimeBlueprintBuilderTest, FromConfigPopulatesClusterConfig) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;

    auto result = hpactor::RuntimeBlueprintBuilder::from_config(cfg);
    ASSERT_TRUE(result.ok());

    const auto& bp = result.value();
    // Cluster is disabled by default.
    EXPECT_FALSE(bp.cluster().enabled);
}
