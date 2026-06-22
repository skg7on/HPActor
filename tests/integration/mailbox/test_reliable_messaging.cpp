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

#include <hpactor/mailbox/in_memory_delivery_store.hpp>
#include <hpactor/mailbox/outbound_tracker.hpp>
#include <hpactor/mailbox/reliable_retry_policy.hpp>
#include <hpactor/net/reliable_ack.hpp>

namespace hpactor::mailbox {

TEST(ReliableMessagingIntegrationTest, FullAckLifecycle) {
    ReliableRetryPolicy policy;
    OutboundTracker tracker(policy);
    ActorAddress target{};
    auto payload = StreamBuffer::from_data(
        reinterpret_cast<const uint8_t*>("test-payload"), 12);
    ASSERT_TRUE(tracker.track(MessageId{1}, target, std::move(payload)));
    EXPECT_EQ(tracker.pending_count(), 1u);
    tracker.on_ack(MessageId{1});
    EXPECT_EQ(tracker.pending_count(), 0u);
}

TEST(ReliableMessagingIntegrationTest, RetryThenSuccess) {
    ReliableRetryPolicy policy{3, std::chrono::milliseconds(100),
                               std::chrono::seconds(10), 2.0};
    OutboundTracker tracker(policy);
    ActorAddress target{};
    auto payload =
        StreamBuffer::from_data(reinterpret_cast<const uint8_t*>("retry-me"), 8);
    tracker.track(MessageId{1}, target, std::move(payload));
    tracker.on_nack(MessageId{1}, std::chrono::milliseconds(100));
    EXPECT_EQ(tracker.pending_count(), 1u);
    tracker.on_ack(MessageId{1});
    EXPECT_EQ(tracker.pending_count(), 0u);
}

TEST(ReliableMessagingIntegrationTest, RetryExhaustion) {
    ReliableRetryPolicy policy{2, std::chrono::milliseconds(10),
                               std::chrono::milliseconds(100), 2.0};
    OutboundTracker tracker(policy);
    ActorAddress target{};
    auto payload =
        StreamBuffer::from_data(reinterpret_cast<const uint8_t*>("exhaust"), 7);
    tracker.track(MessageId{1}, target, std::move(payload));
    tracker.on_nack(MessageId{1}, std::chrono::milliseconds(10));
    tracker.on_nack(MessageId{1}, std::chrono::milliseconds(10));
    tracker.on_nack(MessageId{1}, std::chrono::milliseconds(10));
    EXPECT_EQ(tracker.pending_count(), 0u);
    auto expired = tracker.drain_expired();
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0].message_id, MessageId{1});
}

TEST(ReliableMessagingIntegrationTest, AckWireFormatRoundtrip) {
    net::AckPayload original{MessageId{0xDEADBEEF}, net::AckStatus::Accepted,
                             net::Duration::zero()};
    auto encoded = net::encode_ack(original);
    ASSERT_TRUE(encoded.has_value());
    auto decoded = net::decode_ack(encoded->data(), encoded->size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->message_id, original.message_id);
    EXPECT_EQ(decoded->status, net::AckStatus::Accepted);
}

TEST(ReliableMessagingIntegrationTest, DeliveryStoreSurvivesOutboxCycle) {
    InMemoryDeliveryStore store;
    msg::PendingSend send{MessageId{42}};
    (void)store.put_outbox(send);
    auto loaded = store.load_pending_outbox();
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().size(), 1u);
    (void)store.mark_outbox_complete(MessageId{42});
    auto after = store.load_pending_outbox();
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value().size(), 0u);
}

TEST(ReliableMessagingIntegrationTest, DuplicateDetection) {
    InMemoryDeliveryStore store;
    (void)store.put_inbox(MessageId{42}, 60'000'000'000ULL);
    auto seen = store.seen_inbox(MessageId{42});
    ASSERT_TRUE(seen.ok());
    EXPECT_TRUE(seen.value());
}

} // namespace hpactor::mailbox
