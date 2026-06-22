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
#include <hpactor/net/reliable_ack.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::net {

TEST(ReliableAckTest, AckStatusValuesAreDistinct) {
    EXPECT_NE(static_cast<uint8_t>(AckStatus::Accepted),
              static_cast<uint8_t>(AckStatus::Rejected));
    EXPECT_NE(static_cast<uint8_t>(AckStatus::Rejected),
              static_cast<uint8_t>(AckStatus::Duplicate));
}

TEST(ReliableAckTest, EncodeDecodeRoundtripAccepted) {
    AckPayload original{MessageId{0x12345678ABCDEF00}, AckStatus::Accepted,
                        Duration::zero()};
    auto encoded = encode_ack(original);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(encoded->size(), 14u);

    auto decoded = decode_ack(encoded->data(), encoded->size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->message_id, original.message_id);
    EXPECT_EQ(decoded->status, AckStatus::Accepted);
}

TEST(ReliableAckTest, EncodeDecodeRoundtripRejectedWithRetryAfter) {
    AckPayload original{MessageId{42}, AckStatus::Rejected,
                        std::chrono::milliseconds(500)};
    auto encoded = encode_ack(original);
    ASSERT_TRUE(encoded.has_value());

    auto decoded = decode_ack(encoded->data(), encoded->size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->message_id, original.message_id);
    EXPECT_EQ(decoded->status, AckStatus::Rejected);
    EXPECT_EQ(decoded->retry_after, std::chrono::milliseconds(500));
}

TEST(ReliableAckTest, DecodeRejectsShortBuffer) {
    uint8_t short_buf[5] = {0};
    auto decoded = decode_ack(short_buf, 5);
    EXPECT_FALSE(decoded.has_value());
}

TEST(ReliableAckTest, DecodeRejectsNullData) {
    auto decoded = decode_ack(nullptr, 14);
    EXPECT_FALSE(decoded.has_value());
}

} // namespace hpactor::net
