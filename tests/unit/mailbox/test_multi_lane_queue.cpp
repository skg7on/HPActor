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

#include <hpactor/mailbox/multi_lane_queue.hpp>

#include <gtest/gtest.h>

using namespace hpactor::mailbox;

namespace {

struct TestNode {
    int value = 0;
    std::atomic<TestNode*> mpsc_next{nullptr};
};

} // namespace

class MultiLaneQueueTest : public ::testing::Test {
protected:
    MultiLaneQueue<TestNode> q{4};
};

TEST_F(MultiLaneQueueTest, EnqueueDequeueSingleUserLaneFifo) {
    MultiLaneQueue<TestNode> q1{1};
    TestNode a{10}, b{20}, c{30};
    q1.enqueue(&a, 0);
    q1.enqueue(&b, 0);
    q1.enqueue(&c, 0);
    EXPECT_EQ(q1.dequeue()->value, 10);
    EXPECT_EQ(q1.dequeue()->value, 20);
    EXPECT_EQ(q1.dequeue()->value, 30);
    EXPECT_EQ(q1.dequeue(), nullptr);
}

TEST_F(MultiLaneQueueTest, SystemLaneDrainedFirst) {
    TestNode sys{1}, usr{2};
    q.enqueue(&usr, 0);
    q.enqueue(&sys, MultiLaneQueue<TestNode>::kSystemLaneSentinel);
    EXPECT_EQ(q.dequeue()->value, 1);
    EXPECT_EQ(q.dequeue()->value, 2);
}

TEST_F(MultiLaneQueueTest, UserLaneBeforeSystemStillSystemFirst) {
    TestNode usr{1}, sys{2};
    q.enqueue(&sys, MultiLaneQueue<TestNode>::kSystemLaneSentinel);
    q.enqueue(&usr, 0);
    EXPECT_EQ(q.dequeue()->value, 2);
    EXPECT_EQ(q.dequeue()->value, 1);
}

TEST_F(MultiLaneQueueTest, UserLanesDrainedInPriorityOrder) {
    TestNode p0{0}, p1{10}, p2{20}, p3{30};
    q.enqueue(&p3, 3);
    q.enqueue(&p1, 1);
    q.enqueue(&p2, 2);
    q.enqueue(&p0, 0);
    EXPECT_EQ(q.dequeue()->value, 0);
    EXPECT_EQ(q.dequeue()->value, 10);
    EXPECT_EQ(q.dequeue()->value, 20);
    EXPECT_EQ(q.dequeue()->value, 30);
    EXPECT_EQ(q.dequeue(), nullptr);
}

TEST_F(MultiLaneQueueTest, EmptyWhenAllLanesEmpty) {
    EXPECT_TRUE(q.empty());
    TestNode n{1};
    q.enqueue(&n, 0);
    EXPECT_FALSE(q.empty());
    q.dequeue();
    EXPECT_TRUE(q.empty());
}

TEST_F(MultiLaneQueueTest, EmptyWithSystemLanePopulated) {
    TestNode n{1};
    q.enqueue(&n, MultiLaneQueue<TestNode>::kSystemLaneSentinel);
    EXPECT_FALSE(q.empty());
    q.dequeue();
    EXPECT_TRUE(q.empty());
}

TEST_F(MultiLaneQueueTest, TryDropFromLowestUserLane) {
    TestNode p0{0}, p3{30};
    q.enqueue(&p0, 0);
    q.enqueue(&p3, 3);
    TestNode* dropped = q.try_drop_from_lowest_user_lane();
    ASSERT_NE(dropped, nullptr);
    EXPECT_EQ(dropped->value, 30);
    EXPECT_EQ(q.dequeue()->value, 0);
    EXPECT_EQ(q.dequeue(), nullptr);
}

TEST_F(MultiLaneQueueTest, TryDropFromLowestAllEmpty) {
    EXPECT_EQ(q.try_drop_from_lowest_user_lane(), nullptr);
}

TEST_F(MultiLaneQueueTest, TryDropDoesNotTouchSystemLane) {
    TestNode sys{99};
    q.enqueue(&sys, MultiLaneQueue<TestNode>::kSystemLaneSentinel);
    EXPECT_EQ(q.try_drop_from_lowest_user_lane(), nullptr);
    EXPECT_EQ(q.dequeue()->value, 99);
}

TEST_F(MultiLaneQueueTest, TotalDepthSumsAllLanes) {
    TestNode a{1}, b{2};
    q.enqueue(&a, 0);
    q.enqueue(&b, MultiLaneQueue<TestNode>::kSystemLaneSentinel);
    EXPECT_EQ(q.total_depth(), 2);
    q.dequeue();
    EXPECT_EQ(q.total_depth(), 1);
}

TEST_F(MultiLaneQueueTest, LaneDepthPerLane) {
    TestNode a{1}, b{2}, c{3};
    q.enqueue(&a, 0);
    q.enqueue(&b, 0);
    q.enqueue(&c, 2);
    EXPECT_EQ(q.lane_depth(0), 2);
    EXPECT_EQ(q.lane_depth(1), 0);
    EXPECT_EQ(q.lane_depth(2), 1);
    EXPECT_EQ(
        q.lane_depth(MultiLaneQueue<TestNode>::kSystemLaneSentinel), 0);
}

TEST_F(MultiLaneQueueTest, SetNumUserLanes) {
    EXPECT_EQ(q.num_user_lanes(), 4);
    q.set_num_user_lanes(2);
    EXPECT_EQ(q.num_user_lanes(), 2);
    q.set_num_user_lanes(8);
    EXPECT_EQ(q.num_user_lanes(), 8);
}

TEST_F(MultiLaneQueueTest, PendingFreeSetAndRelease) {
    TestNode n{42};
    q.set_pending_free(&n);
    TestNode* p = q.release_pending_free();
    EXPECT_EQ(p, &n);
    EXPECT_EQ(q.release_pending_free(), nullptr);
}

TEST_F(MultiLaneQueueTest, InjectForTest) {
    TestNode n{7};
    q.inject_for_test(&n, 2);
    EXPECT_EQ(q.lane_depth(2), 1);
    EXPECT_EQ(q.dequeue()->value, 7);
}

TEST_F(MultiLaneQueueTest, ResetClearsAllState) {
    TestNode n{1};
    q.enqueue(&n, 0);
    void* raw = hpactor::mem::allocate(hpactor::mem::RegionType::kMessage,
                                        sizeof(TestNode), hpactor::ActorId{0});
    q.set_pending_free(new (raw) TestNode{5});
    q.reset();
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.total_depth(), 0);
    EXPECT_EQ(q.release_pending_free(), nullptr);
}
