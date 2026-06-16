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

TEST(WorkPlacementSchedulerTest, EdfItemRoutedToEdfQueue) {
    // When edf_scheduled=true, the item must go into the worker edf_queue
    // directly (bypassing the shared-input stack).
    sched::WorkPlacementScheduler placement(1, 4);
    auto& ws = placement.workers()[0];

    sched::WorkItem item;
    item.actor = ActorId{100};
    item.deadline_ns = 5000;
    item.edf_scheduled = true;

    placement.enqueue_admitted(item, 0, /*workers_paused=*/true,
                               [](const sched::WorkItem&) {});

    // EDF queue should contain the item.
    EXPECT_FALSE(ws.edf_queue.empty());
    sched::WorkItem out;
    EXPECT_TRUE(ws.edf_queue.pop(out));
    EXPECT_EQ(out.actor, ActorId{100});
    EXPECT_EQ(out.deadline_ns, 5000);
}

TEST(WorkPlacementSchedulerTest, NonEdfItemGoesToSharedInput) {
    // When edf_scheduled=false, the item must land in the shared-input
    // stack (existing path, no regression).
    sched::WorkPlacementScheduler placement(1, 4);
    auto& ws = placement.workers()[0];

    sched::WorkItem item{ActorId{200}, INT64_MAX, 0};
    item.edf_scheduled = false;

    placement.enqueue_admitted(item, 0, /*workers_paused=*/true,
                               [](const sched::WorkItem&) {});

    // EDF queue should be empty — item went to shared input.
    EXPECT_TRUE(ws.edf_queue.empty());
    // Drain to verify the item landed in the shared-input→ChaseLev path.
    sched::WorkItem out;
    EXPECT_TRUE(placement.pop_local(0, out));
    EXPECT_EQ(out.actor, ActorId{200});
}

TEST(WorkPlacementSchedulerTest, EdfBeforePriorityInPopLocal) {
    // When both EDF and priority items are present, pop_local returns
    // the EDF item first.
    sched::WorkPlacementScheduler placement(1, 4);

    // Enqueue a priority item first (goes to shared-input→ChaseLev).
    sched::WorkItem priority{ActorId{300}, INT64_MAX, 0};
    placement.enqueue_admitted(priority, 0, /*workers_paused=*/true,
                               [](const sched::WorkItem&) {});

    // Then enqueue an EDF item with an explicit deadline.
    sched::WorkItem edf_item{ActorId{301}, 100, 0};
    edf_item.edf_scheduled = true;
    placement.enqueue_admitted(edf_item, 0, /*workers_paused=*/true,
                               [](const sched::WorkItem&) {});

    // EDF item should be popped first regardless of enqueue order.
    sched::WorkItem out;
    EXPECT_TRUE(placement.pop_local(0, out));
    EXPECT_EQ(out.actor, ActorId{301}); // EDF first

    EXPECT_TRUE(placement.pop_local(0, out));
    EXPECT_EQ(out.actor, ActorId{300}); // then priority
}

TEST(WorkPlacementSchedulerTest, RequeuePreservesEdfFlag) {
    // Simulate: EDF item dispatched, actor yields RequeueReady,
    // requeued item should land in EDF again, not ChaseLev.
    sched::WorkPlacementScheduler placement(1, 4);
    auto& ws = placement.workers()[0];

    sched::WorkItem item{ActorId{400}, 7000, 0};
    item.edf_scheduled = true;
    placement.enqueue_admitted(item, 1, /*workers_paused=*/true,
                               [](const sched::WorkItem&) {});

    // Pop it (simulating dispatch).
    sched::WorkItem dispatched;
    ASSERT_TRUE(placement.pop_edf(0, dispatched));
    EXPECT_TRUE(dispatched.edf_scheduled);
    EXPECT_EQ(dispatched.actor, ActorId{400});

    // Simulate requeue with the flag preserved.
    dispatched.sequence = dispatched.sequence + 1;
    placement.enqueue_admitted(dispatched, 1, /*workers_paused=*/true,
                               [](const sched::WorkItem&) {});

    // Should land in EDF again.
    EXPECT_FALSE(ws.edf_queue.empty());
    sched::WorkItem requeued;
    EXPECT_TRUE(ws.edf_queue.pop(requeued));
    EXPECT_TRUE(requeued.edf_scheduled);
    EXPECT_EQ(requeued.actor, ActorId{400});
}
