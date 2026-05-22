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

#include <atomic>
#include <gtest/gtest.h>
#include <hpactor/log/log_event.hpp>
#include <hpactor/log/log_ring_buffer.hpp>
#include <thread>
#include <vector>

using namespace hpactor::log;

static LogEvent make_event() {
    LogEvent evt{};
    evt.level = LogLevel::kInfo;
    evt.category = LogCategory::kUser;
    evt.message = "test";
    return evt;
}

TEST(LogRingBufferTest, PushAndDrain) {
    LogRingBuffer rb(64);
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.size(), 0u);

    auto evt = make_event();
    EXPECT_TRUE(rb.try_push(evt));
    EXPECT_FALSE(rb.empty());
    EXPECT_EQ(rb.size(), 1u);

    int count = 0;
    rb.drain([&](const LogEvent& e) {
        EXPECT_EQ(e.level, LogLevel::kInfo);
        count++;
    });
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(rb.empty());
}

TEST(LogRingBufferTest, OverflowDropsAndCounts) {
    LogRingBuffer rb(4);
    LogEvent evt{};

    for (int i = 0; i < 4; i++) {
        EXPECT_TRUE(rb.try_push(evt));
    }
    EXPECT_FALSE(rb.try_push(evt));
    EXPECT_GT(rb.events_lost(), 0u);
}

TEST(LogRingBufferTest, ConcurrentProducers) {
    LogRingBuffer rb(1024);
    std::atomic<int> total_pushed{0};
    std::atomic<int> total_dropped{0};

    auto producer = [&]() {
        LogEvent evt{};
        for (int i = 0; i < 10000; i++) {
            if (rb.try_push(evt)) {
                total_pushed.fetch_add(1, std::memory_order_relaxed);
            } else {
                total_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread t1(producer);
    std::thread t2(producer);
    std::thread t3(producer);
    t1.join();
    t2.join();
    t3.join();

    int drained = 0;
    rb.drain([&](const LogEvent&) { drained++; });

    EXPECT_EQ(total_pushed.load(), drained);
    EXPECT_EQ(rb.events_lost(), static_cast<uint64_t>(total_dropped.load()));
}

TEST(LogRingBufferTest, DrainAfterOverflowDoesntLosePriorEvents) {
    LogRingBuffer rb(8);
    LogEvent evt{};
    evt.message = "keep";

    for (int i = 0; i < 8; i++) {
        EXPECT_TRUE(rb.try_push(evt));
    }
    EXPECT_FALSE(rb.try_push(evt));
    EXPECT_EQ(rb.events_lost(), 1u);

    int drained = 0;
    rb.drain([&](const LogEvent&) { drained++; });
    EXPECT_EQ(drained, 8);
    EXPECT_TRUE(rb.empty());
}
