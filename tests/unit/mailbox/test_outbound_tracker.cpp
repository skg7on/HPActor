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

#include <gtest/gtest.h>
#include <hpactor/mailbox/outbound_tracker.hpp>
#include <hpactor/mailbox/reliable_retry_policy.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <optional>

namespace hpactor::mailbox {

class OutboundTrackerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        policy_ = ReliableRetryPolicy{};
        tracker_.emplace(policy_);
    }
    ReliableRetryPolicy policy_;
    std::optional<OutboundTracker> tracker_;
    ActorAddress target_{};
};

TEST_F(OutboundTrackerTest, EmptyTrackerHasNoPending) {
    EXPECT_EQ(tracker_->pending_count(), 0);
}

TEST_F(OutboundTrackerTest, TrackReturnsTrue) {
    static const uint8_t kHello[] = {'h', 'e', 'l', 'l', 'o'};
    auto payload = StreamBuffer::from_data(kHello, 5);
    EXPECT_TRUE(tracker_->track(MessageId{1}, target_, std::move(payload)));
    EXPECT_EQ(tracker_->pending_count(), 1);
}

TEST_F(OutboundTrackerTest, AckRemovesEntry) {
    static const uint8_t kHello[] = {'h', 'e', 'l', 'l', 'o'};
    auto payload = StreamBuffer::from_data(kHello, 5);
    tracker_->track(MessageId{1}, target_, std::move(payload));
    tracker_->on_ack(MessageId{1});
    EXPECT_EQ(tracker_->pending_count(), 0);
}

TEST_F(OutboundTrackerTest, NackReschedules) {
    static const uint8_t kHello[] = {'h', 'e', 'l', 'l', 'o'};
    auto payload = StreamBuffer::from_data(kHello, 5);
    auto now = MonotonicClock::now();
    tracker_->track(MessageId{1}, target_, std::move(payload),
                    now + std::chrono::seconds(30));
    tracker_->on_nack(MessageId{1}, std::chrono::milliseconds(200));
    EXPECT_EQ(tracker_->pending_count(), 1);
}

TEST_F(OutboundTrackerTest, AckOnUnknownMessageIsNoop) {
    tracker_->on_ack(MessageId{999});
    EXPECT_EQ(tracker_->pending_count(), 0);
}

TEST_F(OutboundTrackerTest, TrackExceedingCapacityReturnsFalse) {
    for (size_t i = 0; i < OutboundTracker::kMaxPendingPerDestination; ++i) {
        static const uint8_t kX[] = {'x'};
        auto payload = StreamBuffer::from_data(kX, 1);
        EXPECT_TRUE(tracker_->track(MessageId{static_cast<uint64_t>(i + 1)},
                                    target_, std::move(payload)));
    }
    static const uint8_t kOverflow[] = {'o', 'v', 'e', 'r', 'f', 'l', 'o', 'w'};
    auto payload = StreamBuffer::from_data(kOverflow, 8);
    EXPECT_FALSE(
        tracker_->track(MessageId{static_cast<uint64_t>(
                            OutboundTracker::kMaxPendingPerDestination + 1)},
                        target_, std::move(payload)));
}

TEST_F(OutboundTrackerTest, TickDlqsExpiredEntries) {
    static const uint8_t kExpired[] = {'e', 'x', 'p', 'i', 'r', 'e', 'd'};
    auto payload = StreamBuffer::from_data(kExpired, 7);
    auto now = MonotonicClock::now();
    auto deadline = now - std::chrono::seconds(1);
    tracker_->track(MessageId{1}, target_, std::move(payload), deadline);
    tracker_->tick(now + std::chrono::seconds(10));
    EXPECT_EQ(tracker_->pending_count(), 0);
}

TEST_F(OutboundTrackerTest, TickDoesNotRemoveNonExpired) {
    static const uint8_t kAlive[] = {'a', 'l', 'i', 'v', 'e'};
    auto payload = StreamBuffer::from_data(kAlive, 5);
    auto now = MonotonicClock::now();
    auto deadline = now + std::chrono::seconds(60);
    tracker_->track(MessageId{1}, target_, std::move(payload), deadline);
    tracker_->tick(now + std::chrono::seconds(10));
    EXPECT_EQ(tracker_->pending_count(), 1);
}

TEST_F(OutboundTrackerTest, FailPendingForNodeDlqsAll) {
    static const uint8_t kMsg1[] = {'m', 's', 'g', '1'};
    static const uint8_t kMsg2[] = {'m', 's', 'g', '2'};
    auto payload1 = StreamBuffer::from_data(kMsg1, 4);
    auto payload2 = StreamBuffer::from_data(kMsg2, 4);
    tracker_->track(MessageId{1}, target_, std::move(payload1));
    tracker_->track(MessageId{2}, target_, std::move(payload2));
    // Matching the endpoint string produced by node_id() on this platform.
    // The default ActorAddress stores 127.0.0.1:0 as Ipv4Endpoint{0x7F000001,
    // 0}; inet_ntoa(addr) on little-endian produces "1.0.0.127:0".
    tracker_->fail_pending_for_node("1.0.0.127:0");
    EXPECT_EQ(tracker_->pending_count(), 0);
}

} // namespace hpactor::mailbox
