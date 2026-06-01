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

// tests/unit/adt/test_chaselev_deque.cpp
#include <gtest/gtest.h>
#include <hpactor/adt/chaselev_deque.hpp>
#include <random>
#include <thread>
#include <vector>

struct Item {
    int value;
};

TEST(ChaselevDequeTest, BasicPushPop) {
    hpactor::adt::ChaselevDeque<Item> deque;
    EXPECT_EQ(deque.size_approx(), 0U);

    deque.push_bottom(Item{42});
    EXPECT_EQ(deque.size_approx(), 1U);

    Item out{0};
    bool popped = deque.pop_bottom(out);
    EXPECT_TRUE(popped);
    EXPECT_EQ(out.value, 42);
    EXPECT_EQ(deque.size_approx(), 0U);
}

TEST(ChaselevDequeTest, StealReturnsFalseOnEmpty) {
    hpactor::adt::ChaselevDeque<Item> deque;
    Item out{0};
    bool stolen = deque.steal_top(out);
    EXPECT_FALSE(stolen);
}

TEST(ChaselevDequeTest, FillBeyondCapacityAndDrain) {
    hpactor::adt::ChaselevDeque<Item> deque(4); // small initial capacity
    for (int i = 0; i < 128; ++i) {
        deque.push_bottom(Item{i});
    }
    EXPECT_EQ(deque.size_approx(), 128U);
    // Chase-Lev pop_bottom is LIFO: newest item is popped first
    Item out{0};
    for (int i = 127; i >= 0; --i) {
        deque.pop_bottom(out);
        EXPECT_EQ(out.value, i);
    }
    EXPECT_EQ(deque.size_approx(), 0U);
}

TEST(ChaselevDequeTest, ConcurrentPushBottomAndStealTop) {
    hpactor::adt::ChaselevDeque<Item> deque;
    std::atomic<bool> start{false};
    std::atomic<int> steal_count{0};
    std::atomic<int> push_count{0};

    Item out{0};
    std::thread thief([&]() {
        while (!start.load(std::memory_order_acquire)) { /* spin */
        }
        for (int i = 0; i < 1000; ++i) {
            if (deque.steal_top(out)) {
                steal_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (int i = 0; i < 1000; ++i) {
        deque.push_bottom(Item{i});
        push_count.fetch_add(1, std::memory_order_relaxed);
    }
    thief.join();

    // All 1000 items either popped by owner or stolen
    int owner_drained = 0;
    while (deque.pop_bottom(out)) {
        ++owner_drained;
    }
    int total = steal_count.load() + owner_drained;
    EXPECT_EQ(total, push_count.load());
}
