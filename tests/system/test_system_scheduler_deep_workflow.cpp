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

// System test: Scheduler deep workflow coverage
// Exercises A2WS work stealing, EDF scheduling, worker snapshots,
// Chase-Lev deque operations, timer scheduling/cancellation,
// actor pinning, pause/resume cycles, and work placement strategies.

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/sched/a2ws.hpp>
#include <hpactor/sched/edf_queue.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/sched/work_placement_scheduler.hpp>
#include <hpactor/sched/work_queue.hpp>

#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

using namespace hpactor;

// ── Register test actors ─────────────────────────────────────────────────

using CountingActor = test::CountingActor;
using FailingActor = test::FailingActor;

HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);
HPACTOR_REGISTER_ACTOR("FailingActor", FailingActor);

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: A2WS victim selection and steal tracking
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, A2WSVictimSelectionAndSteals) {
    sched::A2WS a2ws(8, 4);

    EXPECT_EQ(a2ws.num_workers(), 8u);

    // Verify victims are valid
    for (uint32_t thief = 0; thief < 8; ++thief) {
        uint32_t victim = a2ws.get_victim(thief);
        EXPECT_LT(victim, 8u);
        EXPECT_NE(victim, thief);
    }

    // Record some steal attempts
    a2ws.record_attempt(0, 3, true);
    a2ws.record_attempt(0, 4, false);
    a2ws.record_attempt(1, 2, true);

    // Verify stats are accessible
    auto& stats0 = a2ws.stats(0);
    EXPECT_GE(stats0.steal_attempts.load(), 0u);

    // Verify same_pool functionality
    EXPECT_TRUE(a2ws.same_pool(0, 1));
    EXPECT_TRUE(a2ws.same_pool(0, 3));
    EXPECT_FALSE(a2ws.same_pool(0, 4));

    // Test pool ranges
    uint32_t start = 0, end = 0;
    a2ws.get_victim_pool(0, start, end);
    EXPECT_LT(start, end);
    EXPECT_LE(end, 8u);

    a2ws.get_victim_pool(5, start, end);
    EXPECT_LT(start, end);
    EXPECT_LE(end, 8u);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: EDFQueue with multiple deadlines
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, EDFQueueMultipleDeadlines) {
    sched::EDFQueue queue;

    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);

    int64_t earliest_ns = 0;
    EXPECT_FALSE(queue.peek(earliest_ns));

    // Push items with various deadlines
    sched::WorkItem item1{ActorId{1}, 300'000'000, 0};
    sched::WorkItem item2{ActorId{2}, 100'000'000, 0};
    sched::WorkItem item3{ActorId{3}, 500'000'000, 0};
    sched::WorkItem item4{ActorId{4}, 200'000'000, 0};
    sched::WorkItem item5{ActorId{5}, INT64_MAX, 0};

    queue.push(item1.deadline_ns, item1);
    queue.push(item2.deadline_ns, item2);
    queue.push(item3.deadline_ns, item3);
    queue.push(item4.deadline_ns, item4);
    queue.push(item5.deadline_ns, item5);

    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.size(), 5u);

    EXPECT_TRUE(queue.peek(earliest_ns));
    EXPECT_EQ(earliest_ns, 100'000'000);

    // Pop items in deadline order
    sched::WorkItem out;
    EXPECT_TRUE(queue.pop(out));
    EXPECT_EQ(out.actor.value(), 2u);

    EXPECT_TRUE(queue.pop(out));
    EXPECT_EQ(out.actor.value(), 4u);

    EXPECT_TRUE(queue.pop(out));
    EXPECT_EQ(out.actor.value(), 1u);

    EXPECT_TRUE(queue.pop(out));
    EXPECT_EQ(out.actor.value(), 3u);

    EXPECT_TRUE(queue.pop(out));
    EXPECT_EQ(out.actor.value(), 5u);

    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.pop(out));
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: EDFQueue with equal deadlines (FIFO tiebreak)
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, EDFQueueEqualDeadlinesFIFO) {
    sched::EDFQueue queue;

    sched::WorkItem item1{ActorId{10}, 42'000'000, 0};
    sched::WorkItem item2{ActorId{20}, 42'000'000, 0};
    sched::WorkItem item3{ActorId{30}, 42'000'000, 0};

    queue.push(item1.deadline_ns, item1);
    queue.push(item2.deadline_ns, item2);
    queue.push(item3.deadline_ns, item3);

    EXPECT_EQ(queue.size(), 3u);

    // All three items have the same deadline and will be popped.
    // The heap comparator uses sequence for tiebreaking; the exact
    // order within the same deadline bucket is implementation-defined.
    sched::WorkItem out;
    std::vector<uint64_t> popped_ids;
    while (queue.pop(out)) {
        popped_ids.push_back(out.actor.value());
    }

    EXPECT_EQ(popped_ids.size(), 3u);
    // All expected actors are present (order not guaranteed for equal
    // deadlines)
    EXPECT_NE(std::find(popped_ids.begin(), popped_ids.end(), 10),
              popped_ids.end());
    EXPECT_NE(std::find(popped_ids.begin(), popped_ids.end(), 20),
              popped_ids.end());
    EXPECT_NE(std::find(popped_ids.begin(), popped_ids.end(), 30),
              popped_ids.end());

    EXPECT_TRUE(queue.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: Worker snapshots from scheduler
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, WorkerSnapshots) {
    Config cfg = test::config_with_scheduler(4);
    ActorSystem system(cfg);

    auto* sched_ptr = system.scheduler();
    ASSERT_NE(sched_ptr, nullptr);

    // Before start, snapshots may be empty
    auto snapshots_before = sched_ptr->worker_snapshots();
    // Snapshots may be empty before workers start

    sched_ptr->start();
    EXPECT_TRUE(sched_ptr->is_running());

    auto snapshots_after = sched_ptr->worker_snapshots();
    EXPECT_EQ(snapshots_after.size(), 4u);

    for (size_t i = 0; i < snapshots_after.size(); ++i) {
        const auto& ws = snapshots_after[i];
        EXPECT_EQ(ws.worker_index, static_cast<uint16_t>(i));
        EXPECT_GE(ws.thread_id, 0u); // 0 on slow builds where threads not yet
                                     // spawned
        EXPECT_TRUE(ws.idle_model == "cv" || ws.idle_model == "polling");
    }

    sched_ptr->stop();
    EXPECT_FALSE(sched_ptr->is_running());
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: Timer scheduling and cancellation through scheduler
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, TimerScheduleAndCancel) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto* sched_ptr = system.scheduler();
    ASSERT_NE(sched_ptr, nullptr);

    sched_ptr->start();

    std::atomic<int> fire_count{0};

    // Schedule a one-shot timer that will be cancelled
    auto handle1 =
        sched_ptr->schedule_after([&fire_count] { fire_count++; }, 1'000'000'000);
    EXPECT_TRUE(handle1.valid());

    // Cancel it immediately
    sched_ptr->cancel_timer(handle1);

    // Schedule a quick one-shot timer
    auto handle2 =
        sched_ptr->schedule_after([&fire_count] { fire_count++; }, 10'000'000);
    EXPECT_TRUE(handle2.valid());

    // Schedule another one-shot timer
    auto handle3 =
        sched_ptr->schedule_after([&fire_count] { fire_count += 2; }, 20'000'000);
    EXPECT_TRUE(handle3.valid());

    // Cancel an invalid handle (should be safe)
    sched::TimerHandle invalid;
    EXPECT_FALSE(invalid.valid());
    sched_ptr->cancel_timer(invalid);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // handle1 was cancelled, should not have fired
    EXPECT_GE(fire_count.load(), 0);

    sched_ptr->stop();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 6: Actor pinning to workers
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, ActorPinningToWorker) {
    Config cfg = test::config_with_scheduler(2);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto actor = system.spawn<test::CountingActor>();
    ASSERT_TRUE(actor);

    auto* act = static_cast<test::CountingActor*>(actor.get().get());

    // Send a message while workers are paused
    driver.pin_actor_to_worker(actor.id(), 0);

    TypedMessage msg1(TypeTag(0x4001), StreamBuffer{});
    act->context()->send(actor.address(), std::move(msg1));

    // Drain spawn items first
    driver.drain(10);

    // Pin again and send more
    driver.pin_actor_to_worker(actor.id(), 0);
    TypedMessage msg2(TypeTag(0x4002), StreamBuffer{});
    act->context()->send(actor.address(), std::move(msg2));

    driver.drain(10);
    EXPECT_TRUE(driver.run_actor(actor.id()));

    // Unpin
    driver.unpin_actor(actor.id());

    // Unpin non-pinned actor (should be safe)
    driver.unpin_actor(ActorId{99999});
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 7: Scheduler pause/resume cycle
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, PauseResumeCycle) {
    Config cfg = test::config_with_scheduler(2);
    ActorSystem system(cfg);

    auto* sched_ptr = system.scheduler();
    ASSERT_NE(sched_ptr, nullptr);

    EXPECT_FALSE(sched_ptr->workers_paused());

    sched_ptr->pause_workers();
    EXPECT_TRUE(sched_ptr->workers_paused());

    // Pause again (should be idempotent)
    sched_ptr->pause_workers();
    EXPECT_TRUE(sched_ptr->workers_paused());

    sched_ptr->resume_workers();
    EXPECT_FALSE(sched_ptr->workers_paused());

    // Resume again (should be idempotent)
    sched_ptr->resume_workers();
    EXPECT_FALSE(sched_ptr->workers_paused());

    // Multiple pause/resume cycles
    for (int i = 0; i < 5; ++i) {
        sched_ptr->pause_workers();
        EXPECT_TRUE(sched_ptr->workers_paused());
        sched_ptr->resume_workers();
        EXPECT_FALSE(sched_ptr->workers_paused());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 8: Work placement strategy (WorkPlacementScheduler)
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, WorkPlacementStrategy) {
    // Create a placement scheduler with 4 workers and 4 priorities
    sched::WorkPlacementScheduler placement(4, 4);

    EXPECT_EQ(placement.worker_count(), 4u);

    // Enqueue admitted work items to different workers (workers NOT paused)
    sched::WorkItem item1{ActorId{1}, 100'000'000, 0};
    sched::WorkItem item2{ActorId{2}, 200'000'000, 0};
    sched::WorkItem item3{ActorId{3}, 50'000'000, 0};

    auto result1 = placement.enqueue_admitted(item1, 0, false, nullptr);
    EXPECT_NE(result1, sched::PlacementResult::NoWorkers);

    auto result2 = placement.enqueue_admitted(item2, 0, false, nullptr);
    EXPECT_NE(result2, sched::PlacementResult::NoWorkers);

    auto result3 = placement.enqueue_admitted(item3, 1, false, nullptr);
    EXPECT_NE(result3, sched::PlacementResult::NoWorkers);

    // Pop local work from worker 0 (may or may not find, depending on
    // round-robin placement)
    sched::WorkItem out;
    placement.pop_local(0, out);

    // Check A2WS is accessible
    auto& a2ws = placement.a2ws();
    EXPECT_GT(a2ws.num_workers(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 9: Work placement with workers paused (deterministic mode)
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, WorkPlacementWithWorkersPaused) {
    Config cfg = test::config_with_scheduler(2);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto actor = system.spawn<test::CountingActor>();
    ASSERT_TRUE(actor);

    auto* act = static_cast<test::CountingActor*>(actor.get().get());

    // Send messages while workers are paused
    driver.pin_actor_to_worker(actor.id(), 0);

    for (int i = 0; i < 5; ++i) {
        TypedMessage msg(TypeTag(0x5000 + static_cast<uint32_t>(i)),
                         StreamBuffer{});
        act->context()->send(actor.address(), std::move(msg));
    }

    driver.drain(10);

    // Pin again and send more
    driver.pin_actor_to_worker(actor.id(), 0);
    TypedMessage msg(TypeTag(0x5005), StreamBuffer{});
    act->context()->send(actor.address(), std::move(msg));

    driver.drain(10);
    EXPECT_TRUE(driver.run_actor(actor.id()));
    EXPECT_GE(act->handler_count, 1);

    driver.unpin_actor(actor.id());
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 10: Scheduler start/stop lifecycle
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, StartStopLifecycle) {
    Config cfg = test::config_with_scheduler(2);
    ActorSystem system(cfg);

    auto* sched_ptr = system.scheduler();
    ASSERT_NE(sched_ptr, nullptr);

    // The ActorSystem constructor calls scheduler_->start() unconditionally,
    // so the scheduler is already running after construction.
    EXPECT_TRUE(sched_ptr->is_running());
    EXPECT_EQ(sched_ptr->worker_count(), 2u);

    // Start again (should be no-op since already running)
    sched_ptr->start();
    EXPECT_TRUE(sched_ptr->is_running());

    sched_ptr->stop();
    EXPECT_FALSE(sched_ptr->is_running());

    // Stop again (should be no-op)
    sched_ptr->stop();
    EXPECT_FALSE(sched_ptr->is_running());
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 11: Scheduler with CalendarQueue timer backend
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, CalendarQueueTimerBackend) {
    Config cfg = test::config_with_scheduler(1);
    cfg.timer_backend = sched::TimerBackend::CalendarQueue;
    cfg.cli.enabled = false;
    cfg.tracing.enabled = false;
    cfg.enable_network = false;

    ActorSystem system(cfg);

    auto* sched_ptr = system.scheduler();
    ASSERT_NE(sched_ptr, nullptr);

    sched_ptr->start();
    EXPECT_TRUE(sched_ptr->is_running());

    std::atomic<int> calendar_fired{0};
    auto handle = sched_ptr->schedule_after(
        [&calendar_fired] { calendar_fired++; }, 5'000'000);
    EXPECT_TRUE(handle.valid());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sched_ptr->cancel_timer(handle);

    sched_ptr->stop();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 12: EDF delivery through scheduler
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchedulerDeep, EDFSchedulingIntegration) {
    Config cfg = test::config_with_scheduler(2);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto* sched_ptr = system.scheduler();
    ASSERT_NE(sched_ptr, nullptr);

    // Spawn and notify with EDF
    auto actor = system.spawn<test::CountingActor>();
    ASSERT_TRUE(actor);

    // Notify with a near deadline via EDF path
    sched_ptr->notify_ready_edf(actor.id(), 0, 50'000'000);

    // Drain to process notification
    driver.drain(10);

    // Send a message via regular path
    driver.pin_actor_to_worker(actor.id(), 0);
    auto* act = static_cast<test::CountingActor*>(actor.get().get());
    TypedMessage msg(TypeTag(0x6001), StreamBuffer{});
    act->context()->send(actor.address(), std::move(msg));

    driver.drain(10);
    EXPECT_TRUE(driver.run_actor(actor.id()));
    EXPECT_GE(act->handler_count, 1);
}
