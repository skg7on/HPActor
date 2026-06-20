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
#include <hpactor/msg/typed_message.hpp>

#include <cstdint>
#include <string>

TEST(TypedMessageTest, DefaultConstruction) {
    hpactor::TypedMessage msg;
    EXPECT_EQ(msg.type_id(), hpactor::TypeTag::Invalid);
    EXPECT_TRUE(msg.payload().empty());
    EXPECT_EQ(msg.parsed(), nullptr);
}

TEST(TypedMessageTest, ConstructionFromTagAndPayload) {
    hpactor::StreamBuffer data = {0x01, 0x02, 0x03};
    hpactor::TypedMessage msg2(hpactor::TypeTag::User, data);
    EXPECT_EQ(msg2.type_id(), hpactor::TypeTag::User);
    EXPECT_EQ(msg2.payload().size(), 3);
}

TEST(TypedMessageTest, MoveSemantics) {
    hpactor::StreamBuffer data = {0x01, 0x02, 0x03};
    hpactor::TypedMessage msg2(hpactor::TypeTag::User, data);
    hpactor::TypedMessage msg3 = std::move(msg2);
    EXPECT_EQ(msg3.type_id(), hpactor::TypeTag::User);
    EXPECT_EQ(msg3.payload().size(), 3);
}

// ── MEM-006: Message Inlining tests ────────────────────────────

struct SmallPod {
    uint64_t a;
    uint32_t b;
}; // 12 bytes
struct TinyPod {
    uint32_t x;
}; // 4 bytes
struct LargePod {
    uint8_t data[64];
}; // 64 bytes
struct NonTrivial {
    std::string s;
}; // not trivially copyable

static_assert(sizeof(SmallPod) <= 32);
static_assert(sizeof(LargePod) > 32);

TEST(MessageInlining, SmallTriviallyCopyableTypeCanInline) {
    EXPECT_TRUE((hpactor::kCanInlinePayload<SmallPod>));
}

TEST(MessageInlining, TinyTypeCanInline) {
    EXPECT_TRUE((hpactor::kCanInlinePayload<TinyPod>));
}

TEST(MessageInlining, LargeTypeCannotInline) {
    EXPECT_FALSE((hpactor::kCanInlinePayload<LargePod>));
}

TEST(MessageInlining, NonTrivialTypeCannotInline) {
    EXPECT_FALSE((hpactor::kCanInlinePayload<NonTrivial>));
}

TEST(MessageInlining, MaxInlinePayloadConstant) {
    EXPECT_EQ(hpactor::kMaxInlinePayload, 32u);
}

TEST(MessageInlining, CreateInlineSmallPayload) {
    uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    auto msg =
        hpactor::TypedMessage::create_inline(hpactor::TypeTag::User, data, 16);
    EXPECT_TRUE(msg.is_inline());
    EXPECT_EQ(msg.inline_size(), 16u);
    EXPECT_EQ(std::memcmp(msg.inline_data(), data, 16), 0);
    // payload() is empty for inline messages (no heap allocation)
    EXPECT_TRUE(msg.payload().empty());
}

TEST(MessageInlining, CreateInlineLargePayloadFallsBack) {
    uint8_t data[64] = {};
    auto msg =
        hpactor::TypedMessage::create_inline(hpactor::TypeTag::User, data, 64);
    EXPECT_FALSE(msg.is_inline());
    EXPECT_EQ(msg.inline_size(), 0u);
    // payload() has the heap-allocated data
    EXPECT_EQ(msg.payload().size(), 64u);
    EXPECT_EQ(std::memcmp(msg.payload().data(), data, 64), 0);
}

TEST(MessageInlining, CreateInlineZeroSize) {
    auto msg =
        hpactor::TypedMessage::create_inline(hpactor::TypeTag::User, nullptr, 0);
    EXPECT_TRUE(msg.is_inline());
    EXPECT_EQ(msg.inline_size(), 0u);
}

TEST(MessageInlining, MoveInlinePreservesPayload) {
    uint8_t data[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    auto msg1 =
        hpactor::TypedMessage::create_inline(hpactor::TypeTag::User, data, 8);
    EXPECT_TRUE(msg1.is_inline());

    auto msg2 = std::move(msg1);
    EXPECT_TRUE(msg2.is_inline());
    EXPECT_EQ(msg2.inline_size(), 8u);
    EXPECT_EQ(std::memcmp(msg2.inline_data(), data, 8), 0);
    // Moved-from should not be inline
    EXPECT_FALSE(msg1.is_inline());
}

TEST(MessageInlining, DefaultConstructedNotInline) {
    hpactor::TypedMessage msg;
    EXPECT_FALSE(msg.is_inline());
    EXPECT_EQ(msg.inline_size(), 0u);
}
