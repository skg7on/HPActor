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
#include <hpactor/mem/telemetry_ring_buffer.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace hpactor::mem;

TEST(TelemetryRingBufferTest, BasicPushDrain) {
    TelemetryRingBuffer<16> rb;
    EXPECT_TRUE(rb.empty());

    AllocationEvent evt{};
    evt.actor_id = 42;
    evt.event_type = 0;

    for (int i = 0; i < 10; ++i) {
        evt.block_size = static_cast<uint16_t>(i);
        EXPECT_TRUE(rb.try_push(evt));
    }
    EXPECT_EQ(rb.size(), 10U);

    // Drain and verify order
    int count = 0;
    rb.drain([&](const AllocationEvent& e) {
        EXPECT_EQ(e.block_size, static_cast<uint16_t>(count));
        EXPECT_EQ(e.actor_id, 42U);
        ++count;
    });
    EXPECT_EQ(count, 10);
    EXPECT_TRUE(rb.empty());
}

TEST(TelemetryRingBufferTest, BufferFullBehavior) {
    TelemetryRingBuffer<16> rb;
    AllocationEvent evt{};
    for (size_t i = 0; i < 16; ++i) {
        EXPECT_TRUE(rb.try_push(evt));
    }
    EXPECT_EQ(rb.size(), 16U);
    EXPECT_FALSE(rb.try_push(evt)); // should fail when full
}

TEST(TelemetryRingBufferTest, ConcurrentMultiProducerWithConsumer) {
    TelemetryRingBuffer<1024> rb;
    constexpr int kEventsPerThread = 500;
    constexpr int kThreads = 4;
    constexpr int kTotal = kEventsPerThread * kThreads;

    std::atomic<int> total_drained{0};
    std::atomic<bool> done{false};

    // Consumer thread drains continuously
    std::thread consumer([&]() {
        while (!done.load(std::memory_order_acquire) || !rb.empty()) {
            rb.drain([&](const AllocationEvent&) { total_drained.fetch_add(1); });
        }
    });

    // Producer threads
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&rb, t]() {
            for (int i = 0; i < kEventsPerThread; ++i) {
                AllocationEvent evt{};
                evt.actor_id = static_cast<uint32_t>(t);
                evt.block_size = static_cast<uint16_t>(i);
                while (!rb.try_push(evt)) {
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
