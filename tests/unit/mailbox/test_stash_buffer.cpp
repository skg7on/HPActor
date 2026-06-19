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

#include <hpactor/mailbox/stash_buffer.hpp>
#include <hpactor/msg/typed_message.hpp>

namespace hpactor {
namespace {

// ── Helper: construct a simple TypedMessage for testing ──────────

TypedMessage make_test_msg(uint32_t tag = 1) {
    StreamBuffer payload;
    payload.resize(4, 0);
    return TypedMessage(static_cast<TypeTag>(tag), std::move(payload));
}

// ═════════════════════════════════════════════════════════════════
// StashBuffer construction
// ═════════════════════════════════════════════════════════════════

TEST(StashBufferTest, DefaultConstructedIsEmpty) {
    StashBuffer buf(16);
    EXPECT_TRUE(buf.empty());
    EXPECT_FALSE(buf.full());
    EXPECT_EQ(buf.size(), 0);
    EXPECT_EQ(buf.capacity(), 16);
}

TEST(StashBufferTest, ZeroCapacityIsValid) {
    StashBuffer buf(0);
    EXPECT_TRUE(buf.empty());
    EXPECT_TRUE(buf.full());
    EXPECT_EQ(buf.capacity(), 0);
}

// ═════════════════════════════════════════════════════════════════
// StashBuffer::try_stash
// ═════════════════════════════════════════════════════════════════

TEST(StashBufferTest, TryStashAddsMessage) {
    StashBuffer buf(4);
    auto msg = make_test_msg(100);
    bool ok = buf.try_stash(std::move(msg));
    EXPECT_TRUE(ok);
    EXPECT_EQ(buf.size(), 1);
    EXPECT_FALSE(buf.empty());
}

TEST(StashBufferTest, TryStashMultipleMessages) {
    StashBuffer buf(4);
    EXPECT_TRUE(buf.try_stash(make_test_msg(1)));
    EXPECT_TRUE(buf.try_stash(make_test_msg(2)));
    EXPECT_TRUE(buf.try_stash(make_test_msg(3)));
    EXPECT_EQ(buf.size(), 3);
}

TEST(StashBufferTest, TryStashRejectsWhenFull) {
    StashBuffer buf(2);
    EXPECT_TRUE(buf.try_stash(make_test_msg(1)));
    EXPECT_TRUE(buf.try_stash(make_test_msg(2)));
    EXPECT_TRUE(buf.full());
    EXPECT_FALSE(buf.try_stash(make_test_msg(3)));
    EXPECT_EQ(buf.size(), 2);
}

TEST(StashBufferTest, TryStashPreservesMessageContent) {
    StashBuffer buf(4);
    buf.try_stash(make_test_msg(42));
    buf.try_stash(make_test_msg(99));

    // Unstash and verify tags (tested via unstash_one)
    // For now verify size
    EXPECT_EQ(buf.size(), 2);
}

// ═════════════════════════════════════════════════════════════════
// StashBuffer::clear
// ═════════════════════════════════════════════════════════════════

TEST(StashBufferTest, ClearEmptiesBuffer) {
    StashBuffer buf(4);
    buf.try_stash(make_test_msg(1));
    buf.try_stash(make_test_msg(2));
    buf.try_stash(make_test_msg(3));
    EXPECT_EQ(buf.size(), 3);

    buf.clear();
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.size(), 0);
}

TEST(StashBufferTest, ClearOnEmptyIsNoOp) {
    StashBuffer buf(4);
    buf.clear();
    EXPECT_TRUE(buf.empty());
}

// ═════════════════════════════════════════════════════════════════
// StashBuffer::unstash_one (pop without delivery)
// ═════════════════════════════════════════════════════════════════

TEST(StashBufferTest, UnstashOneReturnsMessage) {
    StashBuffer buf(4);
    buf.try_stash(make_test_msg(77));

    auto msg = buf.unstash_one();
    EXPECT_TRUE(msg.has_value());
    EXPECT_EQ(static_cast<uint32_t>(msg->type_id()), 77);
    EXPECT_TRUE(buf.empty());
}

TEST(StashBufferTest, UnstashOneReturnsFifoOrder) {
    StashBuffer buf(4);
    buf.try_stash(make_test_msg(1));
    buf.try_stash(make_test_msg(2));
    buf.try_stash(make_test_msg(3));

    auto m1 = buf.unstash_one();
    auto m2 = buf.unstash_one();
    auto m3 = buf.unstash_one();

    EXPECT_TRUE(m1.has_value());
    EXPECT_EQ(static_cast<uint32_t>(m1->type_id()), 1);
    EXPECT_EQ(static_cast<uint32_t>(m2->type_id()), 2);
    EXPECT_EQ(static_cast<uint32_t>(m3->type_id()), 3);
    EXPECT_TRUE(buf.empty());
}

TEST(StashBufferTest, UnstashOneOnEmptyReturnsNullopt) {
    StashBuffer buf(4);
    auto msg = buf.unstash_one();
    EXPECT_FALSE(msg.has_value());
}

// ═════════════════════════════════════════════════════════════════
// StashBuffer::unstash_all (pop all without delivery)
// ═════════════════════════════════════════════════════════════════

TEST(StashBufferTest, UnstashAllReturnsAllMessages) {
    StashBuffer buf(4);
    buf.try_stash(make_test_msg(10));
    buf.try_stash(make_test_msg(20));
    buf.try_stash(make_test_msg(30));

    auto msgs = buf.unstash_all();
    ASSERT_EQ(msgs.size(), 3);
    EXPECT_EQ(static_cast<uint32_t>(msgs[0].type_id()), 10);
    EXPECT_EQ(static_cast<uint32_t>(msgs[1].type_id()), 20);
    EXPECT_EQ(static_cast<uint32_t>(msgs[2].type_id()), 30);
    EXPECT_TRUE(buf.empty());
}

TEST(StashBufferTest, UnstashAllOnEmptyReturnsEmpty) {
    StashBuffer buf(4);
    auto msgs = buf.unstash_all();
    EXPECT_TRUE(msgs.empty());
}

// ═════════════════════════════════════════════════════════════════
// StashBuffer move semantics
// ═════════════════════════════════════════════════════════════════

TEST(StashBufferTest, MoveConstructionPreservesContent) {
    StashBuffer buf(4);
    buf.try_stash(make_test_msg(5));
    buf.try_stash(make_test_msg(6));

    StashBuffer buf2(std::move(buf));
    EXPECT_EQ(buf2.size(), 2);
    EXPECT_EQ(buf2.capacity(), 4);

    auto m = buf2.unstash_one();
    EXPECT_TRUE(m.has_value());
    EXPECT_EQ(static_cast<uint32_t>(m->type_id()), 5);
}

TEST(StashBufferTest, MoveAssignmentPreservesContent) {
    StashBuffer buf1(4);
    buf1.try_stash(make_test_msg(7));

    StashBuffer buf2(8);
    buf2 = std::move(buf1);

    EXPECT_EQ(buf2.size(), 1);
    EXPECT_EQ(buf2.capacity(), 4);

    auto m = buf2.unstash_one();
    EXPECT_TRUE(m.has_value());
    EXPECT_EQ(static_cast<uint32_t>(m->type_id()), 7);
}

// ═════════════════════════════════════════════════════════════════
// Integration: stash during init pattern
// ═════════════════════════════════════════════════════════════════

TEST(StashBufferIntegrationTest, StashDuringInitThenUnstash) {
    // Simulate the classic "stash during initialization" pattern:
    // 1. Actor starts, stashes all messages
    // 2. Initialization completes
    // 3. Stashed messages are replayed

    StashBuffer stash(16);

    // Phase 1: not initialized — stash everything
    auto msg1 = make_test_msg(100);
    auto msg2 = make_test_msg(200);
    auto msg3 = make_test_msg(300);

    // Simulate receive() gating: stash all incoming
    stash.try_stash(std::move(msg1));
    stash.try_stash(std::move(msg2));
    stash.try_stash(std::move(msg3));

    EXPECT_EQ(stash.size(), 3);

    // Phase 2: initialization completes (simulated)

    // Phase 3: replay stashed messages
    auto stashed = stash.unstash_all();
    ASSERT_EQ(stashed.size(), 3);
    EXPECT_EQ(static_cast<uint32_t>(stashed[0].type_id()), 100);
    EXPECT_EQ(static_cast<uint32_t>(stashed[1].type_id()), 200);
    EXPECT_EQ(static_cast<uint32_t>(stashed[2].type_id()), 300);

    EXPECT_TRUE(stash.empty());
}

TEST(StashBufferIntegrationTest, StashRejectsWhenFullDuringInit) {
    StashBuffer stash(2);
    bool rejected = false;

    // Stash up to capacity
    EXPECT_TRUE(stash.try_stash(make_test_msg(1)));
    EXPECT_TRUE(stash.try_stash(make_test_msg(2)));

    // Third message should be rejected
    rejected = !stash.try_stash(make_test_msg(3));
    EXPECT_TRUE(rejected);
    EXPECT_EQ(stash.size(), 2);
}

} // namespace
} // namespace hpactor
