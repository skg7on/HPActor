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

// ── Mock devices config ──────────────────────────────────────────────

TEST_F(AiAcceleratorConfigTest, ParseMockDevices) {
    auto model = parse_fixture("ai_accelerators_mock.toml");
    auto& cfg = model.system.ai_accelerators;
    EXPECT_TRUE(cfg.enabled);
    EXPECT_TRUE(cfg.enable_cpu_probe);
    EXPECT_EQ(cfg.admission_policy,
              hpactor::ai::AdmissionPolicyKind::MostFreeMemory);
    EXPECT_EQ(cfg.lease_ttl_ms, 30000u);

    ASSERT_EQ(cfg.mock_devices.size(), 2u);

    // First mock device
    auto& dev0 = cfg.mock_devices[0];
    EXPECT_EQ(dev0.id, "mock-gpu-0");
    EXPECT_EQ(dev0.kind, hpactor::ai::DeviceKind::Gpu);
    EXPECT_EQ(dev0.vendor, hpactor::ai::DeviceVendor::Mock);
    EXPECT_EQ(dev0.name, "Mock GPU 0");
    EXPECT_EQ(dev0.device_memory_bytes, 24576ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(dev0.compute_units, 100u);
    EXPECT_EQ(dev0.stream_slots, 32u);
    EXPECT_EQ(dev0.health, hpactor::ai::DeviceHealth::Healthy);
    EXPECT_FALSE(dev0.exclusive_only);
    ASSERT_EQ(dev0.labels.size(), 2u);
    EXPECT_EQ(dev0.labels[0].key, "backend");
    EXPECT_EQ(dev0.labels[0].value, "mock");
    EXPECT_EQ(dev0.labels[1].key, "precision");
    EXPECT_EQ(dev0.labels[1].value, "fp16,bf16");

    // Second mock device
    auto& dev1 = cfg.mock_devices[1];
    EXPECT_EQ(dev1.id, "mock-gpu-1");
    EXPECT_EQ(dev1.kind, hpactor::ai::DeviceKind::Gpu);
    EXPECT_EQ(dev1.device_memory_bytes, 12288ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(dev1.compute_units, 50u);
    EXPECT_EQ(dev1.stream_slots, 16u);
}

// ── Invalid configs ──────────────────────────────────────────────────

TEST_F(AiAcceleratorConfigTest, RejectInvalidAdmissionPolicy) {
    EXPECT_TRUE(parse_fails("ai_accelerators_invalid_enum.toml"));
}

TEST_F(AiAcceleratorConfigTest, RejectInvalidTtlBounds) {
    EXPECT_TRUE(parse_fails("ai_accelerators_invalid_ttl.toml"));
}

TEST_F(AiAcceleratorConfigTest, RejectDuplicateMockDeviceIds) {
    EXPECT_TRUE(parse_fails("ai_accelerators_invalid_duplicate_id.toml"));
}

} // namespace
