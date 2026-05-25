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
#include <hpactor/mailbox/overflow_queue.hpp>

#include <thread>
#include <vector>

using namespace hpactor::mailbox;

TEST(OverflowQueueTest, DefaultConstructionEmpty) {
    OverflowQueue<int> q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.depth(), 0);
    EXPECT_EQ(q.max_depth(), 0);

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 0);
    EXPECT_EQ(snap.max_depth, 0);
    EXPECT_EQ(snap.total_pushed, 0);
    EXPECT_EQ(snap.total_popped, 0);
    EXPECT_EQ(snap.total_lost, 0);
}

TEST(OverflowQueueTest, PushPopSingle) {
    OverflowQueue<int> q;
    EXPECT_TRUE(q.try_push(42));
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.depth(), 1);

    int out = 0;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 42);
    EXPECT_TRUE(q.empty());
}

TEST(OverflowQueueTest, PushPopFifoOrder) {
    OverflowQueue<int> q;
    q.try_push(1);
    q.try_push(2);
    q.try_push(3);
    EXPECT_EQ(q.depth(), 3);

    int out = 0;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 1);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 2);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 3);
    EXPECT_TRUE(q.empty());
}

TEST(OverflowQueueTest, TryPopEmptyReturnsFalse) {
    OverflowQueue<int> q;
    int out = 0;
    EXPECT_FALSE(q.try_pop(out));
    EXPECT_EQ(out, 0);
}

TEST(OverflowQueueTest, MaxDepthZeroUnlimited) {
    OverflowQueue<int> q(0);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(q.try_push(std::move(i)));
    }
    EXPECT_EQ(q.depth(), 1000);

    auto snap = q.snapshot();
    EXPECT_EQ(snap.total_pushed, 1000);
    EXPECT_EQ(snap.total_lost, 0);
}

TEST(OverflowQueueTest, MaxDepthEnforcesBoundedCapacity) {
    OverflowQueue<int> q(3);
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_EQ(q.depth(), 3);

    // Fourth push evicts oldest
    EXPECT_TRUE(q.try_push(4));
    EXPECT_EQ(q.depth(), 3);

    auto snap = q.snapshot();
    EXPECT_EQ(snap.total_pushed, 4);
    EXPECT_EQ(snap.total_lost, 1);

    // Oldest (1) was evicted; remaining are 2, 3, 4
    int out = 0;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 2);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 3);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 4);
    EXPECT_TRUE(q.empty());
}

TEST(OverflowQueueTest, SetMaxDepthDynamically) {
    OverflowQueue<int> q(5);
    for (int i = 0; i < 5; ++i) {
        q.try_push(std::move(i));
    }
    EXPECT_EQ(q.depth(), 5);

    // Tighten to 2
    q.set_max_depth(2);
    EXPECT_EQ(q.max_depth(), 2);

    // Next push triggers single eviction (oldest), does not trim to max_depth
    q.try_push(99);
    EXPECT_EQ(q.depth(), 5);

    int out = 0;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 1);  // 0 was evicted
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 2);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 3);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 4);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 99);
    EXPECT_TRUE(q.empty());
}

TEST(OverflowQueueTest, SnapshotCounters) {
    OverflowQueue<int> q(2);
    q.try_push(1);
    q.try_push(2);
    q.try_push(3); // evicts 1

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 2);
    EXPECT_EQ(snap.max_depth, 2);
    EXPECT_EQ(snap.total_pushed, 3);
    EXPECT_EQ(snap.total_popped, 0);
    EXPECT_EQ(snap.total_lost, 1);

    int out = 0;
    q.try_pop(out);

    auto snap2 = q.snapshot();
    EXPECT_EQ(snap2.total_pushed, 3);
    EXPECT_EQ(snap2.total_popped, 1);
    EXPECT_EQ(snap2.total_lost, 1);
}

TEST(OverflowQueueTest, ConcurrentProducerConsumer) {
    OverflowQueue<int> q(0);  // unlimited depth to avoid producer-side eviction
    constexpr int kMessages = 1000;

    std::atomic<int> consumed{0};
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        for (int i = 0; i < kMessages; ++i) {
            q.try_push(std::move(i));
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        int val = 0;
        while (consumed.load(std::memory_order_acquire) < kMessages) {
            if (q.try_pop(val)) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else if (producer_done.load(std::memory_order_acquire)) {
                // TOCTOU: producer may have finished pushing between the
                // failed try_pop and the producer_done load.  Drain any
                // messages that arrived in that window before breaking.
                while (q.try_pop(val)) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
        }
    });

    producer.join();
    consumer.join();

    // After producer + consumer join, all messages should be consumed.
    auto snap = q.snapshot();
    EXPECT_EQ(snap.total_pushed, static_cast<uint64_t>(kMessages));
    EXPECT_EQ(snap.total_popped, static_cast<uint64_t>(kMessages));
    EXPECT_EQ(snap.total_lost, 0);
    EXPECT_TRUE(q.empty());
}

TEST(OverflowQueueTest, MoveSemanticsPreserved) {
    struct MoveOnly {
        int value;
        explicit MoveOnly(int v) : value(v) {}
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&&) = default;
        MoveOnly& operator=(MoveOnly&&) = default;
    };

    OverflowQueue<MoveOnly> q(2);
    EXPECT_TRUE(q.try_push(MoveOnly(10)));
    EXPECT_TRUE(q.try_push(MoveOnly(20)));

    MoveOnly out(0);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.value, 10);
}

TEST(OverflowQueueTest, StringTypeMove) {
    OverflowQueue<std::string> q(3);
    q.try_push(std::string("hello"));
    q.try_push(std::string("world"));

    std::string out;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, "hello");
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, "world");
}