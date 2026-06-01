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

// tests/unit/adt/test_multi_priority_work_queue.cpp
#include <gtest/gtest.h>
#include <hpactor/adt/multi_priority_work_queue.hpp>
#include <vector>

struct TestItem {
    int value;
};

TEST(MultiPriorityWorkQueueTest, PopReturnsFalseOnEmpty) {
    hpactor::adt::MultiPriorityWorkQueue<TestItem> q(4);
    TestItem out{0};
    EXPECT_FALSE(q.pop(out));
}

TEST(MultiPriorityWorkQueueTest, PushPopRoundTrip) {
    hpactor::adt::MultiPriorityWorkQueue<TestItem> q(4);
    TestItem item{42};
    q.push(0, item);
    EXPECT_EQ(q.depth_approx(), 1U);

    TestItem out{0};
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.value, 42);
    EXPECT_EQ(q.depth_approx(), 0U);
}

TEST(MultiPriorityWorkQueueTest, PriorityOrderingHigherFirst) {
    hpactor::adt::MultiPriorityWorkQueue<TestItem> q(4);
    TestItem lo{1};
    TestItem hi{2};
    TestItem mid{3};

    q.push(3, lo);
    q.push(0, hi);
    q.push(2, mid);

    TestItem out{0};
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.value, 2); // high priority first
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.value, 3); // priority 2 before priority 3
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.value, 1); // last remaining
}
