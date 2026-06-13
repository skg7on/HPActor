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

// ── DeviceId ────────────────────────────────────────────────────────
TEST(DeviceIdValidation, ZeroIsInvalid) {
    EXPECT_FALSE(hpactor::ai::is_valid_device_local_id(0));
}

TEST(DeviceIdValidation, NonZeroIsValid) {
    EXPECT_TRUE(hpactor::ai::is_valid_device_local_id(1));
    EXPECT_TRUE(hpactor::ai::is_valid_device_local_id(UINT64_MAX));
}

TEST(DeviceId, DefaultConstruction) {
    hpactor::ai::DeviceId id;
    EXPECT_EQ(id.node_local_id, 0u);
    EXPECT_EQ(id.kind, hpactor::ai::DeviceKind::Cpu);
}

TEST(DeviceId, Equality) {
    hpactor::ai::DeviceId a{1, hpactor::ai::DeviceKind::Gpu};
    hpactor::ai::DeviceId b{1, hpactor::ai::DeviceKind::Gpu};
    hpactor::ai::DeviceId c{2, hpactor::ai::DeviceKind::Gpu};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// ── ResourceQuantities ──────────────────────────────────────────────
TEST(ResourceQuantities, DefaultIsZero) {
    hpactor::ai::ResourceQuantities q;
    EXPECT_EQ(q.device_memory_bytes, 0u);
    EXPECT_EQ(q.host_memory_bytes, 0u);
    EXPECT_EQ(q.compute_units, 0u);
    EXPECT_FALSE(q.exclusive_device);
}

TEST(ResourceQuantities, FitsWithinEmptyRequest) {
    hpactor::ai::ResourceQuantities available;
    available.device_memory_bytes = 1024;
    hpactor::ai::ResourceQuantities request;
    EXPECT_TRUE(request.fits_within(available));
}

TEST(ResourceQuantities, FitsWithinExactFit) {
    hpactor::ai::ResourceQuantities available;
    available.device_memory_bytes = 1024;
    available.compute_units = 8;
    hpactor::ai::ResourceQuantities request;
    request.device_memory_bytes = 1024;
    request.compute_units = 8;
    EXPECT_TRUE(request.fits_within(available));
}

TEST(ResourceQuantities, FitsWithinMemoryExceeded) {
    hpactor::ai::ResourceQuantities available;
    available.device_memory_bytes = 1024;
    hpactor::ai::ResourceQuantities request;
    request.device_memory_bytes = 2048;
    EXPECT_FALSE(request.fits_within(available));
}

TEST(ResourceQuantities, FitsWithinComputeExceeded) {
    hpactor::ai::ResourceQuantities available;
    available.compute_units = 4;
    hpactor::ai::ResourceQuantities request;
    request.compute_units = 8;
    EXPECT_FALSE(request.fits_within(available));
}

TEST(ResourceQuantities, FitsWithinExclusiveConflict) {
    hpactor::ai::ResourceQuantities available;
    available.exclusive_device = true;
    hpactor::ai::ResourceQuantities request;
    request.device_memory_bytes = 100;
    EXPECT_FALSE(request.fits_within(available));
}

TEST(ResourceQuantities, FitsWithinPartialFields) {
    hpactor::ai::ResourceQuantities available;
    available.device_memory_bytes = 1024;
    available.compute_units = 8;
    hpactor::ai::ResourceQuantities request;
    request.device_memory_bytes = 512;
    EXPECT_TRUE(request.fits_within(available));
}

TEST(ResourceQuantities, AddSaturating) {
    hpactor::ai::ResourceQuantities a;
    a.device_memory_bytes = 100;
    hpactor::ai::ResourceQuantities b;
    b.device_memory_bytes = 200;
    auto result = a.add(b);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value().device_memory_bytes, 300u);
}

TEST(ResourceQuantities, AddSaturatingOverflow) {
    hpactor::ai::ResourceQuantities a;
    a.device_memory_bytes = UINT64_MAX;
    hpactor::ai::ResourceQuantities b;
    b.device_memory_bytes = 1;
    auto result = a.add(b);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value().device_memory_bytes, UINT64_MAX);
}

TEST(ResourceQuantities, SubtractNormal) {
    hpactor::ai::ResourceQuantities a;
    a.device_memory_bytes = 300;
    hpactor::ai::ResourceQuantities b;
    b.device_memory_bytes = 100;
    auto result = a.subtract(b);
    EXPECT_TRUE(result.has_value());
}

TEST(ResourceQuantities, SubtractUnderflow) {
    hpactor::ai::ResourceQuantities a;
    a.device_memory_bytes = 100;
    hpactor::ai::ResourceQuantities b;
    b.device_memory_bytes = 200;
    auto result = a.subtract(b);
    EXPECT_FALSE(result.has_value());
}

// ── DeviceSelector ──────────────────────────────────────────────────
TEST(DeviceSelectorValidation, ExactDeviceWithValidId) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::ExactDevice;
    sel.exact_device = hpactor::ai::DeviceId{42, hpactor::ai::DeviceKind::Gpu};
    EXPECT_TRUE(hpactor::ai::is_valid_selector(sel));
}

TEST(DeviceSelectorValidation, ExactDeviceWithZeroId) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::ExactDevice;
    sel.exact_device = hpactor::ai::DeviceId{0, hpactor::ai::DeviceKind::Gpu};
    EXPECT_FALSE(hpactor::ai::is_valid_selector(sel));
}

TEST(DeviceSelectorValidation, AnyIsAlwaysValid) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::Any;
    EXPECT_TRUE(hpactor::ai::is_valid_selector(sel));
}

TEST(DeviceSelectorValidation, KindIsValid) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::Kind;
    sel.required_kind = hpactor::ai::DeviceKind::Gpu;
    EXPECT_TRUE(hpactor::ai::is_valid_selector(sel));
}

TEST(DeviceSelectorValidation, VendorIsValid) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::Vendor;
    sel.required_vendor = hpactor::ai::DeviceVendor::Apple;
    EXPECT_TRUE(hpactor::ai::is_valid_selector(sel));
}

TEST(DeviceSelectorValidation, LabelMatchIsValid) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::LabelMatch;
    sel.required_labels.push_back({"zone", "local"});
    EXPECT_TRUE(hpactor::ai::is_valid_selector(sel));
}

// ── DeviceLeaseRequest ──────────────────────────────────────────────
TEST(DeviceLeaseRequest, DefaultConstruction) {
    hpactor::ai::DeviceLeaseRequest req;
    EXPECT_EQ(req.ttl.count(), 30000);
    EXPECT_EQ(req.priority, 0u);
    EXPECT_FALSE(req.allow_degraded_device);
    EXPECT_FALSE(req.exclusive);
}

// ── DeviceLease ─────────────────────────────────────────────────────
TEST(DeviceLease, DefaultState) {
    hpactor::ai::DeviceLease lease;
    EXPECT_EQ(lease.lease_id, 0u);
    EXPECT_EQ(lease.state, hpactor::ai::LeaseState::Pending);
    EXPECT_EQ(lease.granted_epoch, 0u);
}

// ── DeviceDescriptor ────────────────────────────────────────────────
TEST(DeviceDescriptor, DefaultConstruction) {
    hpactor::ai::DeviceDescriptor desc;
    EXPECT_EQ(desc.id.node_local_id, 0u);
    EXPECT_EQ(desc.kind, hpactor::ai::DeviceKind::Cpu);
    EXPECT_EQ(desc.vendor, hpactor::ai::DeviceVendor::Unknown);
    EXPECT_EQ(desc.health, hpactor::ai::DeviceHealth::Unknown);
}

// ── TypeTag Values ──────────────────────────────────────────────────
#include <hpactor/ai/ai_type_tags.hpp>
#include <hpactor/ai_resource.pb.h>
#include <hpactor/msg/type_tag.hpp>
#include <set>

TEST(TypeTagValues, AllTagsUnique) {
    std::set<uint32_t> values;
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseRequestTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseReplyTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseActivateTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseRenewTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseReleaseTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseRevokedTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiResourceSnapshotRequestTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiResourceSnapshotReplyTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiDeviceSnapshotUpdateTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiNodeResourceSummaryTag));
    EXPECT_EQ(values.size(), 10u);
}

TEST(TypeTagValues, AllInSubsystemRange) {
    auto check = [](hpactor::TypeTag tag) {
        uint32_t v = static_cast<uint32_t>(tag);
        EXPECT_GE(v, 0x80u);
        EXPECT_LE(v, 0x89u);
    };
    check(hpactor::ai::kAiLeaseRequestTag);
    check(hpactor::ai::kAiLeaseReplyTag);
    check(hpactor::ai::kAiLeaseActivateTag);
    check(hpactor::ai::kAiLeaseRenewTag);
    check(hpactor::ai::kAiLeaseReleaseTag);
    check(hpactor::ai::kAiLeaseRevokedTag);
    check(hpactor::ai::kAiResourceSnapshotRequestTag);
    check(hpactor::ai::kAiResourceSnapshotReplyTag);
    check(hpactor::ai::kAiDeviceSnapshotUpdateTag);
    check(hpactor::ai::kAiNodeResourceSummaryTag);
}

TEST(MessageTraits, LeaseRequestTagMatches) {
    EXPECT_EQ(hpactor::MessageTraits<hpactor::PbDeviceLeaseRequest>::tag(),
              hpactor::ai::kAiLeaseRequestTag);
}

TEST(MessageTraits, LeaseReplyTagMatches) {
    EXPECT_EQ(hpactor::MessageTraits<hpactor::PbDeviceLeaseReply>::tag(),
              hpactor::ai::kAiLeaseReplyTag);
}

// ── AcceleratorConfig ───────────────────────────────────────────────
#include <hpactor/ai/accelerator_config.hpp>

TEST(AcceleratorConfig, DefaultsDisabled) {
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

TEST(MockDeviceConfig, Defaults) {
    hpactor::ai::MockDeviceConfig dev;
    EXPECT_EQ(dev.kind, hpactor::ai::DeviceKind::Mock);
    EXPECT_EQ(dev.vendor, hpactor::ai::DeviceVendor::Mock);
    EXPECT_EQ(dev.health, hpactor::ai::DeviceHealth::Healthy);
    EXPECT_FALSE(dev.exclusive_only);
}

// ── NodeResourceSummary ─────────────────────────────────────────────
#include <hpactor/ai/node_resource_summary.hpp>

TEST(NodeResourceSummary, DefaultsNotReady) {
    hpactor::ai::NodeResourceSummary summary;
    EXPECT_FALSE(summary.ready);
    EXPECT_FALSE(summary.draining);
    EXPECT_EQ(summary.ledger_epoch, 0u);
    EXPECT_TRUE(summary.devices.empty());
}

TEST(DeviceCapacitySummary, DefaultConstruction) {
    hpactor::ai::DeviceCapacitySummary dev;
    EXPECT_EQ(dev.active_leases, 0u);
    EXPECT_FALSE(dev.exclusive_in_use);
}

TEST(MessageTraits, AllTenMessagesHaveTraits) {
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseRequest>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseReply>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseActivate>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseRenew>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseRelease>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseRevoked>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbResourceSnapshotRequest>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbResourceSnapshotReply>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceSnapshotUpdate>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbNodeResourceSummary>::tag(),
              hpactor::TypeTag::Invalid);
}
