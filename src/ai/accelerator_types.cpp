// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#include <hpactor/ai/accelerator_types.hpp>
#include <hpactor/types/types.hpp>
#include <limits>

namespace hpactor::ai {

const char* to_string(DeviceKind kind) noexcept {
    switch (kind) {
        case DeviceKind::Cpu:
            return "cpu";
        case DeviceKind::Gpu:
            return "gpu";
        case DeviceKind::Npu:
            return "npu";
        case DeviceKind::Accelerator:
            return "accelerator";
        case DeviceKind::Mock:
            return "mock";
    }
    return "unknown";
}

const char* to_string(DeviceVendor vendor) noexcept {
    switch (vendor) {
        case DeviceVendor::Unknown:
            return "unknown";
        case DeviceVendor::Nvidia:
            return "nvidia";
        case DeviceVendor::Amd:
            return "amd";
        case DeviceVendor::Apple:
            return "apple";
        case DeviceVendor::Intel:
            return "intel";
        case DeviceVendor::Mock:
            return "mock";
    }
    return "unknown";
}

const char* to_string(DeviceHealth health) noexcept {
    switch (health) {
        case DeviceHealth::Unknown:
            return "unknown";
        case DeviceHealth::Healthy:
            return "healthy";
        case DeviceHealth::Degraded:
            return "degraded";
        case DeviceHealth::Unavailable:
            return "unavailable";
        case DeviceHealth::Lost:
            return "lost";
    }
    return "unknown";
}

const char* to_string(LeaseState state) noexcept {
    switch (state) {
        case LeaseState::Pending:
            return "pending";
        case LeaseState::Granted:
            return "granted";
        case LeaseState::Active:
            return "active";
        case LeaseState::Releasing:
            return "releasing";
        case LeaseState::Released:
            return "released";
        case LeaseState::Revoked:
            return "revoked";
        case LeaseState::Expired:
            return "expired";
    }
    return "unknown";
}

const char* to_string(DeviceSelectorKind kind) noexcept {
    switch (kind) {
        case DeviceSelectorKind::Any:
            return "any";
        case DeviceSelectorKind::ExactDevice:
            return "exact_device";
        case DeviceSelectorKind::Kind:
            return "kind";
        case DeviceSelectorKind::Vendor:
            return "vendor";
        case DeviceSelectorKind::LabelMatch:
            return "label_match";
    }
    return "unknown";
}

const char* to_string(AdmissionPolicyKind policy) noexcept {
    switch (policy) {
        case AdmissionPolicyKind::FirstFit:
            return "first_fit";
        case AdmissionPolicyKind::MostFreeMemory:
            return "most_free_memory";
        case AdmissionPolicyKind::ExactDevice:
            return "exact_device";
        case AdmissionPolicyKind::CpuFallback:
            return "cpu_fallback";
    }
    return "unknown";
}

const char* to_string(ResourceAdmissionReason reason) noexcept {
    switch (reason) {
        case ResourceAdmissionReason::Granted:
            return "granted";
        case ResourceAdmissionReason::ResourceActorNotReady:
            return "resource_actor_not_ready";
        case ResourceAdmissionReason::InvalidLeaseRequest:
            return "invalid_lease_request";
        case ResourceAdmissionReason::NoMatchingDevice:
            return "no_matching_device";
        case ResourceAdmissionReason::DeviceUnhealthy:
            return "device_unhealthy";
        case ResourceAdmissionReason::InsufficientDeviceMemory:
            return "insufficient_device_memory";
        case ResourceAdmissionReason::InsufficientHostMemory:
            return "insufficient_host_memory";
        case ResourceAdmissionReason::InsufficientPinnedHostMemory:
            return "insufficient_pinned_host_memory";
        case ResourceAdmissionReason::InsufficientComputeUnits:
            return "insufficient_compute_units";
        case ResourceAdmissionReason::InsufficientStreamSlots:
            return "insufficient_stream_slots";
        case ResourceAdmissionReason::ExclusiveLeaseConflict:
            return "exclusive_lease_conflict";
        case ResourceAdmissionReason::NodeDraining:
            return "node_draining";
        case ResourceAdmissionReason::RejectedByPolicy:
            return "rejected_by_policy";
        case ResourceAdmissionReason::LeaseNotFound:
            return "lease_not_found";
        case ResourceAdmissionReason::LeaseOwnerMismatch:
            return "lease_owner_mismatch";
        case ResourceAdmissionReason::LeaseExpired:
            return "lease_expired";
        case ResourceAdmissionReason::DeviceLost:
            return "device_lost";
        case ResourceAdmissionReason::ProbeSnapshotInvalid:
            return "probe_snapshot_invalid";
    }
    return "unknown";
}

FailureReason to_failure_reason(ResourceAdmissionReason reason) noexcept {
    switch (reason) {
        case ResourceAdmissionReason::Granted:
            return FailureReason::Unknown;
        case ResourceAdmissionReason::ResourceActorNotReady:
            return FailureReason::ActorNotReady;
        case ResourceAdmissionReason::InvalidLeaseRequest:
            return FailureReason::RejectedByPolicy;
        case ResourceAdmissionReason::NoMatchingDevice:
            return FailureReason::NoRoute;
        case ResourceAdmissionReason::DeviceUnhealthy:
            return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::InsufficientDeviceMemory:
            return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::InsufficientHostMemory:
            return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::InsufficientPinnedHostMemory:
            return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::InsufficientComputeUnits:
            return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::InsufficientStreamSlots:
            return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::ExclusiveLeaseConflict:
            return FailureReason::RejectedByPolicy;
        case ResourceAdmissionReason::NodeDraining:
            return FailureReason::Draining;
        case ResourceAdmissionReason::RejectedByPolicy:
            return FailureReason::RejectedByPolicy;
        case ResourceAdmissionReason::LeaseNotFound:
            return FailureReason::NoRoute;
        case ResourceAdmissionReason::LeaseOwnerMismatch:
            return FailureReason::RejectedByPolicy;
        case ResourceAdmissionReason::LeaseExpired:
            return FailureReason::Expired;
        case ResourceAdmissionReason::DeviceLost:
            return FailureReason::NodeUnavailable;
        case ResourceAdmissionReason::ProbeSnapshotInvalid:
            return FailureReason::RejectedByPolicy;
    }
    return FailureReason::Unknown;
}

bool is_valid_device_local_id(DeviceLocalId id) noexcept {
    return id != 0;
}

result<ResourceQuantities>
ResourceQuantities::add(const ResourceQuantities& other) const noexcept {
    ResourceQuantities out;
    auto sat_add_u64 = [](uint64_t a, uint64_t b) {
        if (a > UINT64_MAX - b)
            return UINT64_MAX;
        return a + b;
    };
    auto sat_add_u32 = [](uint32_t a, uint32_t b) {
        if (a > UINT32_MAX - b)
            return UINT32_MAX;
        return a + b;
    };
    out.device_memory_bytes =
        sat_add_u64(device_memory_bytes, other.device_memory_bytes);
    out.host_memory_bytes = sat_add_u64(host_memory_bytes, other.host_memory_bytes);
    out.pinned_host_memory_bytes =
        sat_add_u64(pinned_host_memory_bytes, other.pinned_host_memory_bytes);
    out.compute_units = sat_add_u32(compute_units, other.compute_units);
    out.stream_slots = sat_add_u32(stream_slots, other.stream_slots);
    out.copy_engine_slots = sat_add_u32(copy_engine_slots, other.copy_engine_slots);
    out.kv_cache_bytes = sat_add_u64(kv_cache_bytes, other.kv_cache_bytes);
    out.batch_slots = sat_add_u32(batch_slots, other.batch_slots);
    out.exclusive_device = exclusive_device || other.exclusive_device;
    return result<ResourceQuantities>::make(std::move(out));
}

result<void>
ResourceQuantities::subtract(const ResourceQuantities& other) const noexcept {
    if (other.device_memory_bytes > device_memory_bytes ||
        other.host_memory_bytes > host_memory_bytes ||
        other.pinned_host_memory_bytes > pinned_host_memory_bytes ||
        other.compute_units > compute_units || other.stream_slots > stream_slots ||
        other.copy_engine_slots > copy_engine_slots ||
        other.kv_cache_bytes > kv_cache_bytes || other.batch_slots > batch_slots) {
        return result<void>::make(
            error(errors::invalid_argument, "quantity underflow"));
    }
    return result<void>::make();
}

bool ResourceQuantities::fits_within(const ResourceQuantities& available) const noexcept {
    if (available.exclusive_device)
        return false;
    if (device_memory_bytes > available.device_memory_bytes)
        return false;
    if (host_memory_bytes > available.host_memory_bytes)
        return false;
    if (pinned_host_memory_bytes > available.pinned_host_memory_bytes)
        return false;
    if (compute_units > available.compute_units)
        return false;
    if (stream_slots > available.stream_slots)
        return false;
    if (copy_engine_slots > available.copy_engine_slots)
        return false;
    if (kv_cache_bytes > available.kv_cache_bytes)
        return false;
    if (batch_slots > available.batch_slots)
        return false;
    return true;
}

bool is_valid_selector(const DeviceSelector& sel) noexcept {
    switch (sel.kind) {
        case DeviceSelectorKind::Any:
            return true;
        case DeviceSelectorKind::ExactDevice:
            return is_valid_device_local_id(sel.exact_device.node_local_id);
        case DeviceSelectorKind::Kind:
        case DeviceSelectorKind::Vendor:
            return true;
        case DeviceSelectorKind::LabelMatch:
            return !sel.required_labels.empty();
    }
    return false;
}

} // namespace hpactor::ai
