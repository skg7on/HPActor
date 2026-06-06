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

#include <hpactor/mailbox/backpressure_signal_serialization.hpp>
#include <hpactor/msg/frame.hpp>

using namespace hpactor;

TEST(BackpressureSignalSerializationTest, RoundTripPreservesFields) {
    mailbox::BackpressureSignal signal;
    signal.target = ActorAddress{endpoint_ops::parse_endpoint("127.0.0.1:1111"),
                                 ActorType{7}, ActorId{42}, 3};
    signal.sender = ActorAddress{endpoint_ops::parse_endpoint("127.0.0.1:2222"),
                                 ActorType{8}, ActorId{99}, 4};
    signal.reason = mailbox::BackpressureReason::ByteCapacity;
    signal.depth = 10;
    signal.capacity = 20;
    signal.bytes = 1000;
    signal.byte_capacity = 2000;
    signal.pressure_ratio = 0.5;
    signal.retry_after = std::chrono::milliseconds{250};
    signal.sequence = 123;

    auto encoded = mailbox::serialize_backpressure_signal(
        signal, mailbox::MailboxPressureState::SoftPressure);
    ASSERT_FALSE(encoded.empty());

    auto decoded = mailbox::deserialize_backpressure_signal(encoded);
    ASSERT_TRUE(decoded.has_value());
    if (!decoded)
        return;

    EXPECT_EQ(decoded->signal.target.id, ActorId{42});
    EXPECT_EQ(decoded->signal.sender.id, ActorId{99});
    EXPECT_EQ(decoded->signal.reason, mailbox::BackpressureReason::ByteCapacity);
    EXPECT_EQ(decoded->state, mailbox::MailboxPressureState::SoftPressure);
    EXPECT_EQ(decoded->signal.depth, 10u);
    EXPECT_EQ(decoded->signal.capacity, 20u);
    EXPECT_EQ(decoded->signal.bytes, 1000u);
    EXPECT_EQ(decoded->signal.byte_capacity, 2000u);
    EXPECT_EQ(decoded->signal.retry_after, std::chrono::milliseconds{250});
    EXPECT_EQ(decoded->signal.sequence, 123u);
}
