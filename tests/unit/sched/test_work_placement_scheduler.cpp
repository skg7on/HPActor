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

#include <hpactor/sched/work_placement_scheduler.hpp>

using namespace hpactor;

TEST(WorkPlacementSchedulerTest, EnqueueAdmittedCanBePoppedFromWorker) {
    sched::WorkPlacementScheduler placement(1, 4);
    sched::WorkItem item{ActorId{10}, INT64_MAX, 0};

    auto result =
        placement.enqueue_admitted(item, 0, false, [](const sched::WorkItem&) {});

    EXPECT_EQ(result, sched::PlacementResult::EnqueuedShared);

    sched::WorkItem out;
    EXPECT_TRUE(placement.pop_local(0, out));
    EXPECT_EQ(out.actor, ActorId{10});
}

TEST(WorkPlacementSchedulerTest, DeadlineWorkUsesEdfBeforePriorityQueue) {
    sched::WorkPlacementScheduler placement(1, 4);
    sched::WorkItem priority{ActorId{20}, INT64_MAX, 0};
    sched::WorkItem deadline{ActorId{21}, 42, 1};

    placement.enqueue_admitted(priority, 0, false, [](const sched::WorkItem&) {});
    placement.enqueue_admitted(deadline, 0, false, [](const sched::WorkItem&) {});

    sched::WorkItem out;
    EXPECT_TRUE(placement.pop_local(0, out));
    EXPECT_EQ(out.actor, ActorId{21});
}

TEST(WorkPlacementSchedulerTest, PinnedPausedWorkGoesToPinnedQueue) {
    sched::WorkPlacementScheduler placement(2, 4);
    placement.pin_actor_to_worker(ActorId{30}, 1);

    sched::WorkItem item{ActorId{30}, INT64_MAX, 0};
    auto result =
        placement.enqueue_admitted(item, 0, true, [](const sched::WorkItem&) {});

    EXPECT_EQ(result, sched::PlacementResult::EnqueuedShared);

    sched::WorkItem out;
    uint32_t worker_id = 0;
    EXPECT_TRUE(placement.take_pinned_for_test(ActorId{30}, out, worker_id));
    EXPECT_EQ(worker_id, 1u);
    EXPECT_EQ(out.actor, ActorId{30});
}

TEST(WorkPlacementSchedulerTest, DedicatedThreadSuppressesSharedPlacement) {
    sched::WorkPlacementScheduler placement(1, 4);
    placement.register_dedicated_thread(ActorId{40}, -1);

    bool dedicated_called = false;
    auto result = placement.enqueue_admitted(
        sched::WorkItem{ActorId{40}, INT64_MAX, 0}, 0, false,
        [&dedicated_called](const sched::WorkItem&) { dedicated_called = true; });

    EXPECT_EQ(result, sched::PlacementResult::SuppressedDedicatedThread);
    EXPECT_FALSE(dedicated_called);

    sched::WorkItem out;
    EXPECT_FALSE(placement.pop_local(0, out));
}
