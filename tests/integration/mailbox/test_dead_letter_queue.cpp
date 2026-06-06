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

TEST(DeadLetterQueueTest, RecordStoresPayloadAndTrace) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord dl;
    dl.reason = DeadLetterReason::OverflowPolicy;
    dl.source = DeadLetterSource::MailboxAdmission;
    dl.message_id = 42;
    dl.timestamp_ns = 1000;
    dl.trace_id_hi = 0xABCD;
    dl.trace_id_lo = 0x1234;
    dl.span_id = 0x5678;
    dl.payload_sample = StreamBuffer{0xDE, 0xAD, 0xBE, 0xEF};

    EXPECT_TRUE(q.try_push(std::move(dl)));

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.reason, DeadLetterReason::OverflowPolicy);
    EXPECT_EQ(out.message_id, 42u);
    EXPECT_EQ(out.timestamp_ns, 1000u);
    EXPECT_EQ(out.trace_id_hi, 0xABCDu);
    EXPECT_EQ(out.trace_id_lo, 0x1234u);
    EXPECT_EQ(out.span_id, 0x5678u);
    EXPECT_EQ(out.payload_sample.size(), 4u);
    EXPECT_EQ(out.payload_sample[0], 0xDE);
    EXPECT_EQ(out.payload_sample[3], 0xEF);
}

TEST(DeadLetterQueueTest, ConfigDisabledSkipsPush) {
    DeadLetterConfig cfg;
    cfg.enabled = false;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord dl;
    dl.reason = DeadLetterReason::OverflowPolicy;
    dl.message_id = 1;
    EXPECT_FALSE(q.try_push(std::move(dl)));

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 0u);
}

TEST(DeadLetterQueueTest, ReplayPopsAndReturnsRecord) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord a;
    a.message_id = 1;
    q.try_push(std::move(a));
    DeadLetterRecord b;
    b.message_id = 2;
    q.try_push(std::move(b));
    DeadLetterRecord c;
    c.message_id = 3;
    q.try_push(std::move(c));

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop_at(1, out));
    EXPECT_EQ(out.message_id, 2u);

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 2u);
    EXPECT_EQ(snap.total_popped, 1u);

    DeadLetterRecord r0, r1;
    EXPECT_TRUE(q.try_pop(r0));
    EXPECT_EQ(r0.message_id, 1u);
    EXPECT_TRUE(q.try_pop(r1));
    EXPECT_EQ(r1.message_id, 3u);
}

TEST(DeadLetterQueueTest, ReplayNoPayloadFails) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord a;
    a.reason = DeadLetterReason::OverflowPolicy;
    a.message_id = 1;
    a.payload_sample.clear();
    q.try_push(std::move(a));

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_TRUE(out.payload_sample.empty());
}

TEST(DeadLetterQueueTest, OverflowPreservesOldestWhenFull) {
    DeadLetterConfig cfg;
    cfg.capacity = 3;
    cfg.overflow_policy = DeadLetterOverflowPolicy::DropOldestRecord;
    DeadLetterQueue q(cfg);

    for (uint64_t i = 0; i < 5; ++i) {
        DeadLetterRecord r;
        r.message_id = i;
        q.try_push(std::move(r));
    }

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 3u);
    EXPECT_EQ(snap.total_pushed, 5u);
    EXPECT_EQ(snap.total_lost, 2u);

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.message_id, 2u);
}
