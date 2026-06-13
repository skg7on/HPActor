#include <gtest/gtest.h>
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
