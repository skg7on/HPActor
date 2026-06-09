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

#include <cstring>
#include <gtest/gtest.h>
#include <hpactor/msg/delivery_receipt.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/msg/outbound_delivery_tracker.hpp>
#include <hpactor/msg/retry_policy.hpp>
#include <hpactor/types/types.hpp>

using namespace hpactor;
using namespace hpactor::msg;
using namespace hpactor::mailbox;

namespace {
RetryPolicy default_policy() {
    RetryPolicy p;
    p.max_attempts = 3;
    p.per_attempt_timeout = std::chrono::milliseconds(1000);
    p.backoff = RetryBackoff::Fixed;
    p.initial_backoff = std::chrono::milliseconds(10);
    p.jitter = false;
    return p;
}

uint64_t deadline_ns(uint64_t offset_ms = 5000) {
    return 1'000'000'000ULL + (offset_ms * 1'000'000ULL);
}

StreamBuffer make_frame_data() {
    StreamBuffer buf(16);
    std::memset(buf.data(), 0xAB, 16);
    return buf;
}
} // namespace

// ── Fixture ─────────────────────────────────────────────────────────────────

class OutboundDeliveryTrackerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tracker_ = std::make_unique<OutboundDeliveryTracker>();
    }
    void TearDown() override {
        tracker_.reset();
    }

    std::unique_ptr<OutboundDeliveryTracker> tracker_;
};

// ── track() ────────────────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, TrackReturnsDeliveryReceipt) {
    auto receipt = tracker_->track(make_frame_data(),
                                   endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                                   default_policy(), deadline_ns());
    EXPECT_FALSE(receipt.ready());
    EXPECT_EQ(tracker_->pending(), 1);
}

TEST_F(OutboundDeliveryTrackerTest, TrackAssignsUniqueMessageId) {
    auto r1 = tracker_->track(make_frame_data(),
                              endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                              default_policy(), deadline_ns());
    auto r2 = tracker_->track(make_frame_data(),
                              endpoint_ops::parse_endpoint("127.0.0.1:9001"),
                              default_policy(), deadline_ns());
    EXPECT_NE(r1.message_id(), r2.message_id());
    EXPECT_EQ(tracker_->pending(), 2);
}

// ── on_ack() ────────────────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, OnAckResolvesReceipt) {
    auto receipt = tracker_->track(make_frame_data(),
                                   endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                                   default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    EXPECT_FALSE(receipt.ready());
    tracker_->on_ack(msg_id, endpoint_ops::parse_endpoint("127.0.0.1:9000"));
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::Accepted);
    EXPECT_EQ(tracker_->pending(), 0);
}

TEST_F(OutboundDeliveryTrackerTest, OnAckUnknownMessageIdIsNoop) {
    tracker_->on_ack(MessageId{999}, endpoint_ops::parse_endpoint("127.0.0.1:9000"));
    EXPECT_EQ(tracker_->pending(), 0);
}

// ── on_nack() retryable ─────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, OnNackMailboxFullSchedulesRetry) {
    auto receipt = tracker_->track(make_frame_data(),
                                   endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                                   default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    tracker_->on_nack(msg_id, endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                      static_cast<uint32_t>(DeliveryStatus::MailboxFull), 200);
    EXPECT_FALSE(receipt.ready());
    EXPECT_EQ(tracker_->pending(), 1);
}

// ── on_nack() non-retryable ─────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, OnNackActorDeadResolvesImmediately) {
    auto receipt = tracker_->track(make_frame_data(),
                                   endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                                   default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    tracker_->on_nack(msg_id, endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                      static_cast<uint32_t>(DeliveryStatus::ActorDead), 0);
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::ActorDead);
    EXPECT_EQ(tracker_->pending(), 0);
}

TEST_F(OutboundDeliveryTrackerTest, OnNackExpiredResolvesImmediately) {
    auto receipt = tracker_->track(make_frame_data(),
                                   endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                                   default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    tracker_->on_nack(msg_id, endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                      static_cast<uint32_t>(DeliveryStatus::Expired), 0);
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::Expired);
}

TEST_F(OutboundDeliveryTrackerTest, OnNackDuplicateTreatsAsAck) {
    auto receipt = tracker_->track(make_frame_data(),
                                   endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                                   default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    tracker_->on_nack(msg_id, endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                      static_cast<uint32_t>(DeliveryStatus::Duplicate), 0);
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::Accepted);
}

// ── cancel() ────────────────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, CancelResolvesReceipt) {
    auto receipt = tracker_->track(make_frame_data(),
                                   endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                                   default_policy(), deadline_ns());
    auto msg_id = receipt.message_id();

    tracker_->cancel(msg_id);
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::Cancelled);
    EXPECT_EQ(tracker_->pending(), 0);
}

// ── cancel_endpoint() ───────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, CancelEndpointResolvesAllForThatEndpoint) {
    auto ep1 = endpoint_ops::parse_endpoint("127.0.0.1:9000");
    auto ep2 = endpoint_ops::parse_endpoint("127.0.0.1:9001");

    auto r1 =
        tracker_->track(make_frame_data(), ep1, default_policy(), deadline_ns());
    auto r2 =
        tracker_->track(make_frame_data(), ep1, default_policy(), deadline_ns());
    auto r3 =
        tracker_->track(make_frame_data(), ep2, default_policy(), deadline_ns());

    EXPECT_EQ(tracker_->pending(), 3);

    tracker_->cancel_endpoint(ep1, DeliveryStatus::RemoteUnavailable);

    EXPECT_TRUE(r1.ready());
    EXPECT_EQ(r1.get().status, DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(r2.ready());
    EXPECT_EQ(r2.get().status, DeliveryStatus::RemoteUnavailable);
    EXPECT_FALSE(r3.ready());
    EXPECT_EQ(tracker_->pending(), 1);
}

// ── snapshot() ──────────────────────────────────────────────────────────────

TEST_F(OutboundDeliveryTrackerTest, SnapshotReflectsPending) {
    (void)tracker_->track(make_frame_data(),
                          endpoint_ops::parse_endpoint("127.0.0.1:9000"),
                          default_policy(), deadline_ns());
    (void)tracker_->track(make_frame_data(),
                          endpoint_ops::parse_endpoint("127.0.0.1:9001"),
                          default_policy(), deadline_ns());

    auto snap = tracker_->snapshot();
    EXPECT_EQ(snap.size(), 2);
}
