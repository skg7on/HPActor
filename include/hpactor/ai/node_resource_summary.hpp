// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/ai/accelerator_types.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::ai {

struct DeviceCapacitySummary {
    DeviceId device_id;
    DeviceKind kind{DeviceKind::Cpu};
    DeviceVendor vendor{DeviceVendor::Unknown};
    std::string backend;
    DeviceHealth health{DeviceHealth::Unknown};
    ResourceQuantities allocatable;
    ResourceQuantities reserved;
    ResourceQuantities available;
    uint32_t active_leases{0};
    bool exclusive_in_use{false};
    std::vector<DeviceLabel> labels;
    std::string topology_group;
};

struct NodeResourceSummary {
    EndPoint node_endpoint;
    LedgerEpoch ledger_epoch{0};
    uint64_t generated_at_ns{0};
    bool ready{false};
    bool draining{false};
    std::vector<DeviceCapacitySummary> devices;
};

} // namespace hpactor::ai
