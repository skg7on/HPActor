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
