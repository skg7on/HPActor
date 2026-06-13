// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/ai/accelerator_types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::ai {

struct MockDeviceConfig {
    std::string id;
    DeviceKind kind{DeviceKind::Mock};
    DeviceVendor vendor{DeviceVendor::Mock};
    std::string name;
    uint64_t device_memory_bytes{0};
    uint64_t host_memory_bytes{0};
    uint32_t compute_units{0};
    uint32_t stream_slots{0};
    bool exclusive_only{false};
    DeviceHealth health{DeviceHealth::Healthy};
    std::vector<DeviceLabel> labels;
};

struct AcceleratorConfig {
    bool enabled{false};
    bool enable_cpu_probe{true};
    bool allow_cpu_fallback{true};
    bool allow_empty_inventory{false};
    bool require_resource_plane_ready{false};
    uint32_t probe_interval_ms{1000};
    uint32_t missing_device_grace_ms{5000};
    uint32_t lease_ttl_ms{30000};
    uint32_t min_lease_ttl_ms{1000};
    uint32_t max_lease_ttl_ms{300000};
    AdmissionPolicyKind admission_policy{AdmissionPolicyKind::MostFreeMemory};
    uint64_t cpu_host_memory_budget_bytes{0};
    uint32_t cpu_compute_units{0};
    std::vector<MockDeviceConfig> mock_devices;
};

} // namespace hpactor::ai
