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

// tests/unit/sched/test_multi_priority_work_queue.cpp
#include <gtest/gtest.h>
#include <hpactor/sched/work_queue.hpp>
#include <vector>

TEST(MultiPriorityWorkQueueTest, PopReturnsFalseOnEmpty) {
    hpactor::sched::MultiPriorityWorkQueue q(4);
    hpactor::sched::WorkItem out;
    EXPECT_FALSE(q.pop(out));
}

TEST(MultiPriorityWorkQueueTest, PushPopRoundTrip) {
    hpactor::sched::MultiPriorityWorkQueue q(4);
    hpactor::sched::WorkItem item;
    item.actor = hpactor::ActorId{1};
    item.deadline_ns = 1000;
    item.sequence = 1;

    q.push(0, item);
    EXPECT_EQ(q.depth_approx(), 1U);

    hpactor::sched::WorkItem out;
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.actor.value(), 1U);
    EXPECT_EQ(q.depth_approx(), 0U);
}

TEST(MultiPriorityWorkQueueTest, PriorityOrderingHigherFirst) {
    hpactor::sched::MultiPriorityWorkQueue q(4);
    hpactor::sched::WorkItem lo, hi;
    lo.actor = hpactor::ActorId{1};
    lo.deadline_ns = 2000;
    lo.sequence = 1;
    hi.actor = hpactor::ActorId{2};
    hi.deadline_ns = 1000;
    hi.sequence = 2;

    q.push(3, lo); // low priority (3)
    q.push(0, hi); // high priority (0)
    q.push(2, lo); // also low

    hpactor::sched::WorkItem out;
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.actor.value(), 2U); // high priority first
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.actor.value(), 1U); // priority 2 before priority 3
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.actor.value(), 1U); // last remaining
}
