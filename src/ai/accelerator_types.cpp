// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#include <hpactor/ai/accelerator_types.hpp>

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

} // namespace hpactor::ai
