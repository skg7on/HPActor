// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cstdint>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>
#include <string>
#include <vector>

namespace hpactor::ai {

enum class DeviceKind : uint8_t {
    Cpu = 0,
    Gpu = 1,
    Npu = 2,
    Accelerator = 3,
    Mock = 4,
};

/// Human-readable snake_case string for DeviceKind.
/// Never returns nullptr.
const char* to_string(DeviceKind kind) noexcept;

enum class DeviceVendor : uint8_t {
    Unknown = 0,
    Nvidia = 1,
    Amd = 2,
    Apple = 3,
    Intel = 4,
    Mock = 5,
};

const char* to_string(DeviceVendor vendor) noexcept;

enum class DeviceHealth : uint8_t {
    Unknown = 0,
    Healthy = 1,
    Degraded = 2,
    Unavailable = 3,
    Lost = 4,
};

const char* to_string(DeviceHealth health) noexcept;

enum class LeaseState : uint8_t {
    Pending = 0,
    Granted = 1,
    Active = 2,
    Releasing = 3,
    Released = 4,
    Revoked = 5,
    Expired = 6,
};

const char* to_string(LeaseState state) noexcept;

enum class DeviceSelectorKind : uint8_t {
    Any = 0,
    ExactDevice = 1,
    Kind = 2,
    Vendor = 3,
    LabelMatch = 4,
};

const char* to_string(DeviceSelectorKind kind) noexcept;

enum class AdmissionPolicyKind : uint8_t {
    FirstFit = 0,
    MostFreeMemory = 1,
    ExactDevice = 2,
    CpuFallback = 3,
};

const char* to_string(AdmissionPolicyKind policy) noexcept;

enum class ResourceAdmissionReason : uint16_t {
    Granted = 0,
    ResourceActorNotReady = 1,
    InvalidLeaseRequest = 2,
    NoMatchingDevice = 3,
    DeviceUnhealthy = 4,
    InsufficientDeviceMemory = 5,
    InsufficientHostMemory = 6,
    InsufficientPinnedHostMemory = 7,
    InsufficientComputeUnits = 8,
    InsufficientStreamSlots = 9,
    ExclusiveLeaseConflict = 10,
    NodeDraining = 11,
    RejectedByPolicy = 12,
    LeaseNotFound = 13,
    LeaseOwnerMismatch = 14,
    LeaseExpired = 15,
    DeviceLost = 16,
    ProbeSnapshotInvalid = 17,
};

const char* to_string(ResourceAdmissionReason reason) noexcept;

/// Map ResourceAdmissionReason to the canonical FailureReason.
FailureReason to_failure_reason(ResourceAdmissionReason reason) noexcept;

using DeviceLocalId = uint64_t;

bool is_valid_device_local_id(DeviceLocalId id) noexcept;

struct DeviceId {
    DeviceLocalId node_local_id{0};
    DeviceKind kind{DeviceKind::Cpu};

    bool operator==(const DeviceId&) const noexcept = default;
};

struct ResourceQuantities {
    uint64_t device_memory_bytes{0};
    uint64_t host_memory_bytes{0};
    uint64_t pinned_host_memory_bytes{0};
    uint32_t compute_units{0};
    uint32_t stream_slots{0};
    uint32_t copy_engine_slots{0};
    uint64_t kv_cache_bytes{0};
    uint32_t batch_slots{0};
    bool exclusive_device{false};

    [[nodiscard]] result<ResourceQuantities>
    add(const ResourceQuantities& other) const noexcept;
    [[nodiscard]] result<void>
    subtract(const ResourceQuantities& other) const noexcept;
    [[nodiscard]] bool
    fits_within(const ResourceQuantities& available) const noexcept;
};

struct DeviceLabel {
    std::string key;
    std::string value;
};

struct DeviceSelector {
    DeviceSelectorKind kind{DeviceSelectorKind::Any};
    DeviceId exact_device{};
    DeviceKind required_kind{DeviceKind::Gpu};
    DeviceVendor required_vendor{DeviceVendor::Unknown};
    std::vector<DeviceLabel> required_labels;
    int32_t preferred_numa_node{-1};
    std::string preferred_topology_group;
    bool allow_cpu_fallback{false};
};

bool is_valid_selector(const DeviceSelector& sel) noexcept;

using LeaseId = uint64_t;
using ProbeEpoch = uint64_t;
using LedgerEpoch = uint64_t;

struct DeviceLeaseRequest {
    ActorAddress requester;
    std::string workload_id;
    std::string model_or_job;
    std::string tenant_id;
    ResourceQuantities requested;
    DeviceSelector selector;
    std::chrono::milliseconds ttl{30'000};
    uint32_t priority{0};
    bool allow_degraded_device{false};
    bool exclusive{false};
};

struct DeviceLease {
    LeaseId lease_id{0};
    DeviceId device_id{};
    ActorAddress owner;
    std::string workload_id;
    std::string model_or_job;
    std::string tenant_id;
    ResourceQuantities reserved;
    LeaseState state{LeaseState::Pending};
    LedgerEpoch granted_epoch{0};
    int64_t granted_at_ns{0};
    int64_t expires_at_ns{0};
    uint32_t priority{0};
};

struct DeviceDescriptor {
    DeviceId id;
    DeviceKind kind{DeviceKind::Cpu};
    DeviceVendor vendor{DeviceVendor::Unknown};
    std::string backend;
    std::string name;
    std::string stable_key;
    uint32_t backend_ordinal{0};
    uint64_t parent_node_local_id{0};
    int32_t numa_node{-1};
    std::string topology_group;
    ResourceQuantities total;
    ResourceQuantities allocatable;
    uint64_t capabilities{0};
    DeviceHealth health{DeviceHealth::Unknown};
    std::vector<DeviceLabel> labels;
};

} // namespace hpactor::ai
