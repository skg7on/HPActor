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
#include <hpactor/mem/freelist.hpp>

#include <atomic>
#include <thread>
#include <vector>

struct TestNode {
    TestNode* next;
    uint32_t value;
};

TEST(FreeListTest, BasicPushPopLIFO) {
    hpactor::mem::FreeList<TestNode> fl;
    EXPECT_TRUE(fl.empty());

    TestNode a{nullptr, 1};
    TestNode b{nullptr, 2};
    TestNode c{nullptr, 3};

    fl.push(&a);
    EXPECT_FALSE(fl.empty());
    fl.push(&b);
    fl.push(&c);

    TestNode* n = fl.pop();
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->value, 3U); // LIFO
    n = fl.pop();
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->value, 2U);
    n = fl.pop();
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->value, 1U);
    EXPECT_TRUE(fl.empty());
    EXPECT_EQ(fl.pop(), nullptr);
}

TEST(FreeListTest, ConcurrentPushPopAcrossThreads) {
    hpactor::mem::FreeList<TestNode> fl;
    constexpr int kNumItems = 10000;

    std::atomic<int> push_count{0};
    std::atomic<int> pop_count{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            // Push items
            for (int i = 0; i < kNumItems / 4; ++i) {
                auto* node = new TestNode{nullptr, static_cast<uint32_t>(i)};
                fl.push(node);
                push_count.fetch_add(1);
            }
            // Pop items
            while (pop_count.load() < kNumItems) {
                TestNode* n = fl.pop();
                if (n) {
                    pop_count.fetch_add(1);
                    delete n;
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(pop_count.load(), kNumItems);
    EXPECT_TRUE(fl.empty());
}
