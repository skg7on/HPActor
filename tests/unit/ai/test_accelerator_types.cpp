#include <gtest/gtest.h>
#include <hpactor/ai/accelerator_types.hpp>
#include <hpactor/ai_resource.pb.h>

TEST(ProtoGeneration, LeaseRequestCompiles) {
    hpactor::PbDeviceLeaseRequest req;
    req.set_workload_id("test-workload");
    req.set_ttl_ms(30000);
    EXPECT_EQ(req.workload_id(), "test-workload");
    EXPECT_EQ(req.ttl_ms(), 30000u);
}

TEST(ProtoGeneration, LeaseReplyFields) {
    hpactor::PbDeviceLeaseReply reply;
    reply.set_granted(true);
    reply.set_lease_id(42);
    reply.set_device_local_id(7);
    reply.set_ledger_epoch(1);
    reply.set_reason(0); // Granted
    reply.set_detail("ok");
    reply.set_expires_at_ns(1'000'000'000);
    EXPECT_TRUE(reply.granted());
    EXPECT_EQ(reply.lease_id(), 42u);
}

TEST(ProtoGeneration, AllMessagesInstantiable) {
    // Verify all 11 message types compile and default-construct
    hpactor::PbDeviceLeaseRequest r1;
    hpactor::PbDeviceLeaseReply r2;
    hpactor::PbDeviceLeaseActivate r3;
    hpactor::PbDeviceLeaseRenew r4;
    hpactor::PbDeviceLeaseRelease r5;
    hpactor::PbDeviceLeaseRevoked r6;
    hpactor::PbResourceSnapshotRequest r7;
    hpactor::PbResourceSnapshotReply r8;
    hpactor::PbDeviceSnapshotUpdate r9;
    hpactor::PbDeviceCapacitySummary r10;
    hpactor::PbNodeResourceSummary r11;
    SUCCEED();
}

// ── DeviceKind ──────────────────────────────────────────────────────
TEST(DeviceKindString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceKind::Cpu), "cpu");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceKind::Gpu), "gpu");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceKind::Npu), "npu");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceKind::Accelerator),
                 "accelerator");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceKind::Mock), "mock");
}

// ── DeviceVendor ────────────────────────────────────────────────────
TEST(DeviceVendorString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Unknown),
                 "unknown");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Nvidia),
                 "nvidia");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Amd), "amd");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Apple), "apple");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Intel), "intel");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Mock), "mock");
}

// ── DeviceHealth ────────────────────────────────────────────────────
TEST(DeviceHealthString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceHealth::Unknown),
                 "unknown");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceHealth::Healthy),
                 "healthy");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceHealth::Degraded),
                 "degraded");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceHealth::Unavailable),
                 "unavailable");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceHealth::Lost), "lost");
}

// ── LeaseState ──────────────────────────────────────────────────────
TEST(LeaseStateString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Pending),
                 "pending");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Granted),
                 "granted");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Active), "active");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Releasing),
                 "releasing");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Released),
                 "released");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Revoked),
                 "revoked");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Expired),
                 "expired");
}

// ── DeviceSelectorKind ──────────────────────────────────────────────
TEST(DeviceSelectorKindString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceSelectorKind::Any),
                 "any");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceSelectorKind::ExactDevice),
                 "exact_device");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceSelectorKind::Kind),
                 "kind");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceSelectorKind::Vendor),
                 "vendor");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceSelectorKind::LabelMatch),
                 "label_match");
}

// ── AdmissionPolicyKind ─────────────────────────────────────────────
TEST(AdmissionPolicyKindString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::AdmissionPolicyKind::FirstFit),
                 "first_fit");
    EXPECT_STREQ(
        hpactor::ai::to_string(hpactor::ai::AdmissionPolicyKind::MostFreeMemory),
        "most_free_memory");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::AdmissionPolicyKind::ExactDevice),
                 "exact_device");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::AdmissionPolicyKind::CpuFallback),
                 "cpu_fallback");
}

// ── ResourceAdmissionReason ─────────────────────────────────────────
#include <hpactor/msg/failure_reason.hpp>

TEST(ResourceAdmissionReasonString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::Granted),
                 "granted");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::ResourceActorNotReady),
                 "resource_actor_not_ready");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::InvalidLeaseRequest),
                 "invalid_lease_request");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::NoMatchingDevice),
                 "no_matching_device");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::DeviceUnhealthy),
                 "device_unhealthy");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::InsufficientDeviceMemory),
                 "insufficient_device_memory");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::InsufficientHostMemory),
                 "insufficient_host_memory");
    EXPECT_STREQ(
        hpactor::ai::to_string(
            hpactor::ai::ResourceAdmissionReason::InsufficientPinnedHostMemory),
        "insufficient_pinned_host_memory");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::InsufficientComputeUnits),
                 "insufficient_compute_units");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::InsufficientStreamSlots),
                 "insufficient_stream_slots");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::ExclusiveLeaseConflict),
                 "exclusive_lease_conflict");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::NodeDraining),
                 "node_draining");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::RejectedByPolicy),
                 "rejected_by_policy");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::LeaseNotFound),
                 "lease_not_found");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::LeaseOwnerMismatch),
                 "lease_owner_mismatch");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::LeaseExpired),
                 "lease_expired");
    EXPECT_STREQ(
        hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::DeviceLost),
        "device_lost");
    EXPECT_STREQ(hpactor::ai::to_string(
                     hpactor::ai::ResourceAdmissionReason::ProbeSnapshotInvalid),
                 "probe_snapshot_invalid");
}

TEST(ResourceAdmissionReasonMapping, ToFailureReason) {
    using R = hpactor::ai::ResourceAdmissionReason;
    using F = hpactor::FailureReason;
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::Granted), F::Unknown);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::ResourceActorNotReady),
              F::ActorNotReady);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::InvalidLeaseRequest),
              F::RejectedByPolicy);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::NoMatchingDevice), F::NoRoute);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::DeviceUnhealthy), F::MemoryPressure);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::InsufficientDeviceMemory),
              F::MemoryPressure);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::InsufficientHostMemory),
              F::MemoryPressure);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::ExclusiveLeaseConflict),
              F::RejectedByPolicy);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::NodeDraining), F::Draining);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::LeaseNotFound), F::NoRoute);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::LeaseOwnerMismatch),
              F::RejectedByPolicy);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::LeaseExpired), F::Expired);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::DeviceLost), F::NodeUnavailable);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::ProbeSnapshotInvalid),
              F::RejectedByPolicy);
}
