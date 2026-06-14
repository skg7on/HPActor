// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/ai/accelerator_config.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/config/topology_model.hpp>

#include <gtest/gtest.h>
#include <string>

namespace {

#ifndef TEST_DATA_DIR
#    define TEST_DATA_DIR "tests/data"
#endif

std::string fixture_path(const std::string& name) {
    return std::string(TEST_DATA_DIR) + "/toml/ai/" + name;
}

class AiAcceleratorConfigTest : public ::testing::Test {
  protected:
    hpactor::config::TopologyModel parse_fixture(const std::string& name) {
        auto result = hpactor::config::TomlParser::parse(fixture_path(name));
        EXPECT_TRUE(result.has_value()) << "Parse failed for: " << name;
        return std::move(result.value());
    }

    bool parse_fails(const std::string& name) {
        auto result = hpactor::config::TomlParser::parse(fixture_path(name));
        return !result.has_value();
    }
};

// ── Defaults ─────────────────────────────────────────────────────────

TEST_F(AiAcceleratorConfigTest, ConfigDefaults) {
    hpactor::ai::AcceleratorConfig cfg;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_TRUE(cfg.enable_cpu_probe);
    EXPECT_TRUE(cfg.allow_cpu_fallback);
    EXPECT_FALSE(cfg.allow_empty_inventory);
    EXPECT_FALSE(cfg.require_resource_plane_ready);
    EXPECT_EQ(cfg.probe_interval_ms, 1000u);
    EXPECT_EQ(cfg.missing_device_grace_ms, 5000u);
    EXPECT_EQ(cfg.lease_ttl_ms, 30000u);
    EXPECT_EQ(cfg.min_lease_ttl_ms, 1000u);
    EXPECT_EQ(cfg.max_lease_ttl_ms, 300000u);
    EXPECT_EQ(cfg.admission_policy,
              hpactor::ai::AdmissionPolicyKind::MostFreeMemory);
    EXPECT_EQ(cfg.cpu_host_memory_budget_bytes, 0u);
    EXPECT_EQ(cfg.cpu_compute_units, 0u);
    EXPECT_TRUE(cfg.mock_devices.empty());
}

// ── Disabled config ──────────────────────────────────────────────────

TEST_F(AiAcceleratorConfigTest, ParseDisabled) {
    auto model = parse_fixture("ai_accelerators_disabled.toml");
    EXPECT_FALSE(model.system.ai_accelerators.enabled);
}

// ── CPU-only config ──────────────────────────────────────────────────

TEST_F(AiAcceleratorConfigTest, ParseCpuOnly) {
    auto model = parse_fixture("ai_accelerators_cpu.toml");
    auto& cfg = model.system.ai_accelerators;
    EXPECT_TRUE(cfg.enabled);
    EXPECT_TRUE(cfg.enable_cpu_probe);
    EXPECT_TRUE(cfg.allow_cpu_fallback);
    EXPECT_EQ(cfg.probe_interval_ms, 2000u);
    EXPECT_EQ(cfg.lease_ttl_ms, 60000u);
    EXPECT_EQ(cfg.min_lease_ttl_ms, 5000u);
    EXPECT_EQ(cfg.max_lease_ttl_ms, 120000u);
    EXPECT_EQ(cfg.admission_policy, hpactor::ai::AdmissionPolicyKind::FirstFit);
    EXPECT_EQ(cfg.cpu_host_memory_budget_bytes, 16384ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(cfg.cpu_compute_units, 16u);
    EXPECT_TRUE(cfg.mock_devices.empty());
}

} // namespace
