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

#include <hpactor/msg/dead_letter_record.hpp>

#include <gtest/gtest.h>

namespace hpactor::mailbox {

TEST(DeadLetterQueueTest, ConfigAccessorReturnsReference) {
    DeadLetterConfig cfg;
    cfg.capacity = 1234;
    cfg.enabled = false;
    DeadLetterQueue q(cfg);

    EXPECT_EQ(q.config().capacity, 1234u);
    EXPECT_EQ(q.config().enabled, false);
}

TEST(DeadLetterQueueTest, SnapshotRecordsReturnsCopy) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord a;
    a.message_id = 1;
    q.try_push(std::move(a));
    DeadLetterRecord b;
    b.message_id = 2;
    q.try_push(std::move(b));

    auto records = q.snapshot_records();
    EXPECT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].message_id, 1u);
    EXPECT_EQ(records[1].message_id, 2u);

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 2u);
}

TEST(DeadLetterQueueTest, TryPopAtRemovesCorrectElement) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    for (uint64_t i = 0; i < 5; ++i) {
        DeadLetterRecord r;
        r.message_id = i;
        q.try_push(std::move(r));
    }

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop_at(2, out));
    EXPECT_EQ(out.message_id, 2u);

    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.message_id, 0u);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.message_id, 1u);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.message_id, 3u);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.message_id, 4u);

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 0u);
    EXPECT_EQ(snap.total_popped, 5u);
}

TEST(DeadLetterQueueTest, TryPopAtOutOfBoundsReturnsFalse) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord r;
    r.message_id = 1;
    q.try_push(std::move(r));

    DeadLetterRecord out;
    EXPECT_FALSE(q.try_pop_at(1, out));
    EXPECT_FALSE(q.try_pop_at(100, out));

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 1u);
}

} // namespace hpactor::mailbox
