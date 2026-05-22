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

#include <hpactor/mailbox/dead_letter_queue.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::mailbox;

TEST(DeadLetterQueueTest, PushPopWithOverflow) {
    DeadLetterConfig cfg;
    cfg.capacity = 2;
    cfg.max_payload_sample_bytes = 3;
    cfg.overflow_policy = DeadLetterOverflowPolicy::DropOldestRecord;

    DeadLetterQueue q(cfg);

    DeadLetterRecord a;
    a.reason = DeadLetterReason::ActorNotFound;
    a.source = DeadLetterSource::LocalDelivery;
    a.message_id = 1;
    a.payload_sample = StreamBuffer{1, 2, 3, 4, 5};
    EXPECT_TRUE(q.try_push(std::move(a)));

    DeadLetterRecord b;
    b.reason = DeadLetterReason::MissingRoute;
    b.source = DeadLetterSource::ActorProxy;
    b.message_id = 2;
    EXPECT_TRUE(q.try_push(std::move(b)));

    DeadLetterRecord c;
    c.reason = DeadLetterReason::NetworkPartition;
    c.source = DeadLetterSource::Transport;
    c.message_id = 3;
    EXPECT_TRUE(q.try_push(std::move(c)));

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 2u);
    EXPECT_EQ(snap.total_pushed, 3u);
    EXPECT_EQ(snap.total_lost, 1u);

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.message_id, 2u);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.message_id, 3u);
    EXPECT_FALSE(q.try_pop(out));
}
