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

// tests/unit/sched/test_edf_queue.cpp
#include <gtest/gtest.h>
#include <hpactor/sched/edf_queue.hpp>
#include <hpactor/types/types.hpp>

TEST(EDFQueueTest, EmptyQueue) {
    hpactor::sched::EDFQueue q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0U);
}

TEST(EDFQueueTest, PopOnEmptyReturnsFalse) {
    hpactor::sched::EDFQueue q;
    hpactor::sched::WorkItem item;
    item.actor = hpactor::ActorId{0};
    item.deadline_ns = 0;
    item.sequence = 0;
    EXPECT_FALSE(q.pop(item));
}

TEST(EDFQueueTest, PushPopRoundTrip) {
    hpactor::sched::EDFQueue q;
    hpactor::sched::WorkItem in, out;
    in.actor = hpactor::ActorId{42};
    in.deadline_ns = 1000;
    in.sequence = 1;

    q.push(1000, in);
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.size(), 1U);

    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.actor.value(), 42U);
    EXPECT_EQ(out.deadline_ns, 1000);
    EXPECT_TRUE(q.empty());
}

TEST(EDFQueueTest, EDFOrderingEarlierDeadlineFirst) {
    hpactor::sched::EDFQueue q;
    hpactor::sched::WorkItem early, late;
    early.actor = hpactor::ActorId{1};
    early.deadline_ns = 100;
    early.sequence = 1;

    late.actor = hpactor::ActorId{2};
    late.deadline_ns = 200;
    late.sequence = 2;

    q.push(200, late);  // later deadline
    q.push(100, early); // earlier deadline

    hpactor::sched::WorkItem out;
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.actor.value(), 1U); // early deadline first

    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.actor.value(), 2U); // late deadline second
}

TEST(EDFQueueTest, FIFOOrderingForSameDeadline) {
    hpactor::sched::EDFQueue q;
    hpactor::sched::WorkItem first, second;
    first.actor = hpactor::ActorId{10};
    first.deadline_ns = 500;
    first.sequence = 1;

    second.actor = hpactor::ActorId{20};
    second.deadline_ns = 500;
    second.sequence = 2;

    q.push(500, second);
    q.push(500, first);

    hpactor::sched::WorkItem out;
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.actor.value(), 10U); // first by sequence

    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.actor.value(), 20U); // second by sequence
}

TEST(EDFQueueTest, PeekReturnsEarliestDeadline) {
    hpactor::sched::EDFQueue q;
    int64_t deadline_out;
    EXPECT_FALSE(q.peek(deadline_out));

    q.push(300, hpactor::sched::WorkItem{hpactor::ActorId{5}, 300, 1});
    EXPECT_TRUE(q.peek(deadline_out));
    EXPECT_EQ(deadline_out, 300);
}

TEST(WorkItemTest, EdfScheduledDefaultIsFalse) {
    hpactor::sched::WorkItem item{};
    EXPECT_FALSE(item.edf_scheduled);
}

TEST(WorkItemTest, EdfScheduledExplicitSet) {
    hpactor::sched::WorkItem item;
    item.edf_scheduled = true;
    EXPECT_TRUE(item.edf_scheduled);
}
