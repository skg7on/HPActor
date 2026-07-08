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

#include <hpactor/adt/mpsc_ring_buffer.hpp>

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace hpactor;

// Trivial payload for buffer tests
struct TestPayload {
    int value = 0;
    char pad[28] = {}; // make it 32 bytes, realistic size
};

// Test 1: compile-time capacity, basic push/drain
TEST(MpscRingBufferTest, CompileTimeBasicPushDrain) {
    adt::MpscRingBuffer<TestPayload, 16> rb;
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.size(), 0);

    TestPayload p{42};
    EXPECT_TRUE(rb.try_push(p));
    EXPECT_FALSE(rb.empty());
    EXPECT_EQ(rb.size(), 1);

    int count = 0;
    rb.drain([&](const TestPayload& e) {
        EXPECT_EQ(e.value, 42);
        count++;
    });
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(rb.empty());
}

// Test 2: compile-time, drain ordering
TEST(MpscRingBufferTest, CompileTimeDrainOrdering) {
    adt::MpscRingBuffer<TestPayload, 16> rb;
    for (int i = 0; i < 10; ++i) {
        TestPayload p{i};
        EXPECT_TRUE(rb.try_push(p));
    }
    EXPECT_EQ(rb.size(), 10);

    int expected = 0;
    rb.drain([&](const TestPayload& e) {
        EXPECT_EQ(e.value, expected);
        expected++;
    });
    EXPECT_EQ(expected, 10);
    EXPECT_TRUE(rb.empty());
}

// Test 3: compile-time, overflow + events_lost
TEST(MpscRingBufferTest, CompileTimeOverflow) {
    adt::MpscRingBuffer<TestPayload, 4> rb;
    TestPayload p{1};
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(rb.try_push(p));
    }
    EXPECT_EQ(rb.size(), 4);
    EXPECT_FALSE(rb.try_push(p)); // full — should fail
    EXPECT_EQ(rb.events_lost(), 1);
}

// Test 4: compile-time, drain-after-overflow preserves elements
TEST(MpscRingBufferTest, CompileTimeDrainAfterOverflow) {
    adt::MpscRingBuffer<TestPayload, 8> rb;
    TestPayload p{7};
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(rb.try_push(p));
    }
    EXPECT_FALSE(rb.try_push(p)); // overflow
    EXPECT_EQ(rb.events_lost(), 1);

    int drained = 0;
    rb.drain([&](const TestPayload&) { drained++; });
    EXPECT_EQ(drained, 8);
    EXPECT_TRUE(rb.empty());
}

// Test 5: compile-time, concurrent producers + consumer
TEST(MpscRingBufferTest, CompileTimeConcurrentProducers) {
    adt::MpscRingBuffer<TestPayload, 1024> rb;
    constexpr int kEventsPerThread = 500;
    constexpr int kThreads = 4;
    constexpr int kTotal = kEventsPerThread * kThreads;

    std::atomic<int> total_drained{0};
    std::atomic<bool> done{false};

    std::thread consumer([&]() {
        while (!done.load(std::memory_order_acquire) || !rb.empty()) {
            rb.drain([&](const TestPayload&) {
                total_drained.fetch_add(1, std::memory_order_relaxed);
            });
        }
    });

    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&rb, t]() {
            for (int i = 0; i < kEventsPerThread; ++i) {
                TestPayload p{t * 1000 + i};
                while (!rb.try_push(p)) {
                    // spin until consumer drains
                }
            }
        });
    }

    for (auto& th : producers)
        th.join();
    done.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(total_drained.load(), kTotal);
    EXPECT_TRUE(rb.empty());
}

// Test 6: dynamic capacity, basic push/drain
TEST(MpscRingBufferTest, DynamicBasicPushDrain) {
    adt::DynamicMpscRingBuffer<TestPayload> rb(16);
    EXPECT_TRUE(rb.empty());

    TestPayload p{99};
    EXPECT_TRUE(rb.try_push(p));
    EXPECT_EQ(rb.size(), 1);

    int count = 0;
    rb.drain([&](const TestPayload& e) {
        EXPECT_EQ(e.value, 99);
        count++;
    });
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(rb.empty());
}

// Test 7: dynamic, overflow
TEST(MpscRingBufferTest, DynamicOverflow) {
    adt::DynamicMpscRingBuffer<TestPayload> rb(4);
    TestPayload p{};
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(rb.try_push(p));
    }
    EXPECT_FALSE(rb.try_push(p));
    EXPECT_EQ(rb.events_lost(), 1);
}

// Test 8: dynamic, concurrent producers
TEST(MpscRingBufferTest, DynamicConcurrentProducers) {
    adt::DynamicMpscRingBuffer<TestPayload> rb(1024);
    constexpr int kEventsPerThread = 500;
    constexpr int kThreads = 4;
    constexpr int kTotal = kEventsPerThread * kThreads;

    std::atomic<int> total_drained{0};
    std::atomic<bool> done{false};

    std::thread consumer([&]() {
        while (!done.load(std::memory_order_acquire) || !rb.empty()) {
            rb.drain([&](const TestPayload&) {
                total_drained.fetch_add(1, std::memory_order_relaxed);
            });
        }
    });

    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&rb, t]() {
            for (int i = 0; i < kEventsPerThread; ++i) {
                TestPayload p{t * 1000 + i};
                while (!rb.try_push(p)) {
                    // spin until consumer drains
                }
            }
        });
    }

    for (auto& th : producers)
        th.join();
    done.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(total_drained.load(), kTotal);
}

// Test 9: dynamic, capacity validation
TEST(MpscRingBufferTest, DynamicCapacityValidation) {
    {
        adt::DynamicMpscRingBuffer<TestPayload> rb2(2);
        EXPECT_TRUE(rb2.empty());
    }
    {
        adt::DynamicMpscRingBuffer<TestPayload> rb4(4);
        EXPECT_TRUE(rb4.empty());
    }
    {
        adt::DynamicMpscRingBuffer<TestPayload> rb8(8);
        EXPECT_TRUE(rb8.empty());
    }
    {
        adt::DynamicMpscRingBuffer<TestPayload> rb64(64);
        EXPECT_TRUE(rb64.empty());
    }
    {
        adt::DynamicMpscRingBuffer<TestPayload> rb1024(1024);
        EXPECT_TRUE(rb1024.empty());
    }
}

// Test 10: per-slot publish protocol under stress
// Multiple producers pushing to adjacent slots while consumer drains.
// TSAN will catch any consumer read of an unpublished slot.
TEST(MpscRingBufferTest, PerSlotPublishStress) {
    adt::MpscRingBuffer<TestPayload, 256> rb;
    std::atomic<uint64_t> total_pushed{0};
    std::atomic<uint64_t> total_consumed{0};
    std::atomic<bool> stop{false};

    std::thread consumer([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            rb.drain([&](const TestPayload&) {
                total_consumed.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // final drain
        rb.drain([&](const TestPayload&) {
            total_consumed.fetch_add(1, std::memory_order_relaxed);
        });
    });

    constexpr int kProducers = 8;
    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&rb, &total_pushed]() {
            for (int i = 0; i < 5000; ++i) {
                TestPayload p{i};
                if (rb.try_push(p)) {
                    total_pushed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : producers)
        th.join();
    stop.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(total_consumed.load(), total_pushed.load());
    EXPECT_TRUE(rb.empty());
}

// Test 11: compile-time, drain_up_to bounded (max_items < available) and
// unlimited (max_items > available)
TEST(MpscRingBufferTest, CompileTimeDrainUpTo) {
    adt::MpscRingBuffer<TestPayload, 16> rb;
    // Push 8 items
    for (int i = 0; i < 8; ++i) {
        TestPayload p{i};
        EXPECT_TRUE(rb.try_push(p));
    }
    EXPECT_EQ(rb.size(), 8);

    // Bounded: drain only 5 of 8
    int count = 0;
    size_t drained = rb.drain_up_to(5, [&](const TestPayload& e) {
        EXPECT_EQ(e.value, count);
        count++;
    });
    EXPECT_EQ(drained, 5);
    EXPECT_EQ(count, 5);
    EXPECT_EQ(rb.size(), 3);

    // Unlimited: drain remaining (3 items, but request 10)
    count = 0;
    drained = rb.drain_up_to(10, [&](const TestPayload& e) {
        EXPECT_EQ(e.value, 5 + count);
        count++;
    });
    EXPECT_EQ(drained, 3);
    EXPECT_EQ(count, 3);
    EXPECT_TRUE(rb.empty());
}

// Test 12: dynamic, drain_up_to bounded and unlimited
TEST(MpscRingBufferTest, DynamicDrainUpTo) {
    adt::DynamicMpscRingBuffer<TestPayload> rb(16);
    // Push 8 items
    for (int i = 0; i < 8; ++i) {
        TestPayload p{i};
        EXPECT_TRUE(rb.try_push(p));
    }
    EXPECT_EQ(rb.size(), 8);

    // Bounded: drain only 5 of 8
    int count = 0;
    size_t drained = rb.drain_up_to(5, [&](const TestPayload& e) {
        EXPECT_EQ(e.value, count);
        count++;
    });
    EXPECT_EQ(drained, 5);
    EXPECT_EQ(count, 5);
    EXPECT_EQ(rb.size(), 3);

    // Unlimited: drain remaining (3 items, but request 10)
    count = 0;
    drained = rb.drain_up_to(10, [&](const TestPayload& e) {
        EXPECT_EQ(e.value, 5 + count);
        count++;
    });
    EXPECT_EQ(drained, 3);
    EXPECT_EQ(count, 3);
    EXPECT_TRUE(rb.empty());
}

// Test 13: dynamic, drain_up_to(0) consumes nothing and preserves buffer
TEST(MpscRingBufferTest, DynamicDrainUpToZeroConsumesNothing) {
    adt::DynamicMpscRingBuffer<TestPayload> rb(8);
    ASSERT_TRUE(rb.try_push(TestPayload{7}));
    EXPECT_EQ(rb.drain_up_to(0, [](const TestPayload&) {}), 0u);
    EXPECT_EQ(rb.size(), 1u);
}
