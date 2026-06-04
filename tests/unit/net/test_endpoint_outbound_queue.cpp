// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/net/endpoint_outbound_queue.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <vector>

namespace hpactor::net {
namespace {

// Helper: create a PendingMessage with given payload size
PendingMessage make_msg(size_t payload_size = 100) {
    PendingMessage msg;
    msg.data = StreamBuffer(payload_size, 0xAA);
    msg.enqueued_at = std::chrono::steady_clock::now();
    return msg;
}

bool is_accepted(TransportSendResult r) {
    return r == TransportSendResult::Sent;
}

bool is_queue_full(TransportSendResult r) {
    return r == TransportSendResult::QueueFull;
}

class EndpointOutboundQueueTest : public ::testing::Test {
  protected:
    EndpointOutboundLimits limits;
    void SetUp() override {
        limits.max_messages = 100;
        limits.max_bytes = 100 * 1024; // 100 KiB
        limits.control_lane_reserve = 10;
        limits.reliable_headroom_pct = 0.20;
    }
};

TEST_F(EndpointOutboundQueueTest, AcceptsUnderLimit) {
    EndpointOutboundQueue q(limits);
    auto r = q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                           TypeTag::User);
    EXPECT_TRUE(is_accepted(r));
    EXPECT_EQ(q.total_messages(), 1u);
    EXPECT_EQ(q.data_messages(), 1u);
    EXPECT_EQ(q.control_messages(), 0u);
}

TEST_F(EndpointOutboundQueueTest, RejectsAtMessageLimit) {
    limits.max_messages = 3;
    limits.control_lane_reserve = 0;
    limits.reliable_headroom_pct = 0.0;
    EndpointOutboundQueue q(limits);
    EXPECT_TRUE(is_accepted(q.try_enqueue(
        make_msg(50), mailbox::DeliveryMode::BestEffort, TypeTag::User)));
    EXPECT_TRUE(is_accepted(q.try_enqueue(
        make_msg(50), mailbox::DeliveryMode::BestEffort, TypeTag::User)));
    EXPECT_TRUE(is_accepted(q.try_enqueue(
        make_msg(50), mailbox::DeliveryMode::BestEffort, TypeTag::User)));
    auto r = q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                           TypeTag::User);
    EXPECT_TRUE(is_queue_full(r));
    EXPECT_EQ(q.total_messages(), 3u);
}

TEST_F(EndpointOutboundQueueTest, RejectsAtByteLimit) {
    limits.max_messages = 1000;
    limits.max_bytes = 100;
    limits.control_lane_reserve = 0;
    limits.reliable_headroom_pct = 0.0;
    EndpointOutboundQueue q(limits);
    EXPECT_TRUE(is_accepted(q.try_enqueue(
        make_msg(60), mailbox::DeliveryMode::BestEffort, TypeTag::User)));
    auto r = q.try_enqueue(make_msg(60), mailbox::DeliveryMode::BestEffort,
                           TypeTag::User);
    EXPECT_TRUE(is_queue_full(r));
}

TEST_F(EndpointOutboundQueueTest, ControlLaneHasHardReserve) {
    limits.max_messages = 10;
    limits.control_lane_reserve = 3;
    limits.reliable_headroom_pct = 0.0;
    EndpointOutboundQueue q(limits);
    // Fill data lane completely (no reliable headroom)
    size_t data_effective = limits.max_messages - limits.control_lane_reserve;
    for (size_t i = 0; i < data_effective; ++i) {
        EXPECT_TRUE(is_accepted(q.try_enqueue(
            make_msg(50), mailbox::DeliveryMode::BestEffort, TypeTag::User)));
    }
    // Data lane should now be full
    auto r = q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                           TypeTag::User);
    EXPECT_TRUE(is_queue_full(r));
    // But control messages still accepted (within reserve)
    EXPECT_TRUE(is_accepted(q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                                          TypeTag::SpawnRequestTag)));
    EXPECT_EQ(q.control_messages(), 1u);
}

TEST_F(EndpointOutboundQueueTest, ReliableHeadroomReserved) {
    limits.max_messages = 10;
    limits.control_lane_reserve = 0;
    limits.reliable_headroom_pct = 0.30;
    EndpointOutboundQueue q(limits);
    // Best-effort cutoff = 10 * 0.70 = 7
    for (size_t i = 0; i < 7; ++i) {
        EXPECT_TRUE(is_accepted(q.try_enqueue(
            make_msg(50), mailbox::DeliveryMode::BestEffort, TypeTag::User)));
    }
    // Next best-effort should be rejected
    auto r_be = q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                              TypeTag::User);
    EXPECT_TRUE(is_queue_full(r_be));
    // But at-least-once should still be accepted (up to 10)
    EXPECT_TRUE(is_accepted(q.try_enqueue(
        make_msg(50), mailbox::DeliveryMode::AtLeastOnce, TypeTag::User)));
    EXPECT_TRUE(is_accepted(q.try_enqueue(
        make_msg(50), mailbox::DeliveryMode::AtLeastOnce, TypeTag::User)));
    EXPECT_TRUE(is_accepted(q.try_enqueue(
        make_msg(50), mailbox::DeliveryMode::AtLeastOnce, TypeTag::User)));
    // 10th message fills it
    auto r_rel = q.try_enqueue(make_msg(50), mailbox::DeliveryMode::AtLeastOnce,
                               TypeTag::User);
    EXPECT_TRUE(is_queue_full(r_rel));
}

TEST_F(EndpointOutboundQueueTest, ReliableRejectedBeyondEffective) {
    limits.max_messages = 5;
    limits.control_lane_reserve = 0;
    limits.reliable_headroom_pct = 0.0;
    EndpointOutboundQueue q(limits);
    // Fill completely
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(is_accepted(q.try_enqueue(
            make_msg(50), mailbox::DeliveryMode::AtLeastOnce, TypeTag::User)));
    }
    auto r = q.try_enqueue(make_msg(50), mailbox::DeliveryMode::AtLeastOnce,
                           TypeTag::User);
    EXPECT_TRUE(is_queue_full(r));
}

TEST_F(EndpointOutboundQueueTest, DequeuePrefersControl) {
    EndpointOutboundQueue q(limits);
    // Enqueue data first, then control
    q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort, TypeTag::User);
    q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                  TypeTag::SpawnRequestTag);
    // Control should be dequeued first
    auto first = q.try_dequeue();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(q.control_messages(), 0u); // control lane should be drained first
    EXPECT_EQ(q.data_messages(), 1u);    // data lane still has one
}

TEST_F(EndpointOutboundQueueTest, PressureTransitionsToSoft) {
    limits.max_messages = 100;
    limits.high_watermark = 0.70;
    limits.low_watermark = 0.50;
    limits.critical_watermark = 0.90;
    limits.control_lane_reserve = 0;
    limits.reliable_headroom_pct = 0.0;
    EndpointOutboundQueue q(limits);
    for (size_t i = 0; i < 71; ++i) {
        q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                      TypeTag::User);
    }
    EXPECT_EQ(q.pressure_state(), mailbox::MailboxPressureState::SoftPressure);
}

TEST_F(EndpointOutboundQueueTest, PressureTransitionsToHard) {
    limits.max_messages = 100;
    limits.high_watermark = 0.70;
    limits.low_watermark = 0.50;
    limits.critical_watermark = 0.90;
    limits.control_lane_reserve = 0;
    limits.reliable_headroom_pct = 0.0;
    EndpointOutboundQueue q(limits);
    for (size_t i = 0; i < 91; ++i) {
        q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                      TypeTag::User);
    }
    EXPECT_EQ(q.pressure_state(), mailbox::MailboxPressureState::HardPressure);
}

TEST_F(EndpointOutboundQueueTest, PressureRecoversToNormal) {
    limits.max_messages = 100;
    limits.high_watermark = 0.70;
    limits.low_watermark = 0.50;
    limits.critical_watermark = 0.90;
    limits.control_lane_reserve = 0;
    limits.reliable_headroom_pct = 0.0;
    EndpointOutboundQueue q(limits);
    for (size_t i = 0; i < 80; ++i) {
        q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                      TypeTag::User);
    }
    EXPECT_EQ(q.pressure_state(), mailbox::MailboxPressureState::SoftPressure);
    for (size_t i = 0; i < 60; ++i) {
        q.try_dequeue();
    }
    EXPECT_EQ(q.pressure_state(), mailbox::MailboxPressureState::Normal);
}

TEST_F(EndpointOutboundQueueTest, ByteTrackingAccurate) {
    EndpointOutboundQueue q(limits);
    q.try_enqueue(make_msg(200), mailbox::DeliveryMode::BestEffort, TypeTag::User);
    EXPECT_EQ(q.data_messages(), 1u);
    EXPECT_GE(q.total_bytes(), 200u);
    q.try_dequeue();
    EXPECT_EQ(q.data_messages(), 0u);
    EXPECT_EQ(q.total_bytes(), 0u);
}

TEST_F(EndpointOutboundQueueTest, SnapshotReturnsCounts) {
    EndpointOutboundQueue q(limits);
    q.try_enqueue(make_msg(100), mailbox::DeliveryMode::BestEffort, TypeTag::User);
    q.try_enqueue(make_msg(50), mailbox::DeliveryMode::BestEffort,
                  TypeTag::SpawnRequestTag);
    auto snap = q.snapshot();
    EXPECT_EQ(snap.data_messages, 1u);
    EXPECT_EQ(snap.control_messages, 1u);
}

} // anonymous namespace
} // namespace hpactor::net
