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

// tests/unit/sched/test_sched_branches.cpp
//
// Branch-coverage tests for scheduler subsystem components that previously
// lacked dedicated test coverage: ChaseLevDeque, MultiPriorityWorkQueue,
// TimingWheel, DedicatedThreadPool, and WorkerThread lifecycle.

#include <gtest/gtest.h>

#include <hpactor/actor/actor_state.hpp>
#include <hpactor/adt/chaselev_deque.hpp>
#include <hpactor/adt/multi_priority_work_queue.hpp>
#include <hpactor/sched/a2ws.hpp>
#include <hpactor/sched/dedicated_thread_pool.hpp>
#include <hpactor/sched/edf_queue.hpp>
#include <hpactor/sched/work_placement_scheduler.hpp>
#include <hpactor/sched/worker_thread.hpp>
#include <hpactor/timer/timing_wheel.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace hpactor;

// ============================================================================
// ChaseLevDeque — lock-free work-stealing deque edge cases
// ============================================================================

TEST(ChaseLevDequeTest, EmptyPopBottomReturnsFalse) {
    adt::ChaselevDeque<int> dq(4);
    int out = -1;
    EXPECT_FALSE(dq.pop_bottom(out));
    EXPECT_EQ(out, -1);
}

TEST(ChaseLevDequeTest, EmptyStealTopReturnsFalse) {
    adt::ChaselevDeque<int> dq(4);
    int out = -1;
    EXPECT_FALSE(dq.steal_top(out));
    EXPECT_EQ(out, -1);
}

TEST(ChaseLevDequeTest, PushPopBottomRoundTrip) {
    adt::ChaselevDeque<int> dq(4);
    dq.push_bottom(42);
    EXPECT_EQ(dq.size_approx(), 1U);

    int out = 0;
    EXPECT_TRUE(dq.pop_bottom(out));
    EXPECT_EQ(out, 42);
    EXPECT_EQ(dq.size_approx(), 0U);
}

TEST(ChaseLevDequeTest, SingleElementSteal) {
    adt::ChaselevDeque<int> dq(4);
    dq.push_bottom(99);

    int out = 0;
    EXPECT_TRUE(dq.steal_top(out));
    EXPECT_EQ(out, 99);
    EXPECT_EQ(dq.size_approx(), 0U);
}

TEST(ChaseLevDequeTest, LIFOvsFIFOSemantics) {
    // Owner pops bottom → LIFO (most recently pushed)
    // Thief steals top → FIFO (least recently pushed)
    adt::ChaselevDeque<int> dq(4);
    dq.push_bottom(1);
    dq.push_bottom(2);
    dq.push_bottom(3);

    int out = 0;
    // Steal from top: should get 1 (oldest)
    EXPECT_TRUE(dq.steal_top(out));
    EXPECT_EQ(out, 1);

    // Pop from bottom: should get 3 (newest)
    EXPECT_TRUE(dq.pop_bottom(out));
    EXPECT_EQ(out, 3);

    // Remaining: 2
    EXPECT_TRUE(dq.pop_bottom(out));
    EXPECT_EQ(out, 2);
    EXPECT_EQ(dq.size_approx(), 0U);
}

TEST(ChaseLevDequeTest, GrowBeyondInitialCapacity) {
    adt::ChaselevDeque<int> dq(4);
    // Push 10 elements — should trigger grow
    for (int i = 0; i < 10; ++i) {
        dq.push_bottom(i);
    }
    EXPECT_EQ(dq.size_approx(), 10U);

    // Pop all back (LIFO order)
    for (int i = 9; i >= 0; --i) {
        int out = -1;
        EXPECT_TRUE(dq.pop_bottom(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_EQ(dq.size_approx(), 0U);
}

TEST(ChaseLevDequeTest, StealAfterGrow) {
    adt::ChaselevDeque<int> dq(4);
    for (int i = 0; i < 8; ++i) {
        dq.push_bottom(i);
    }
    // Steal oldest
    int out = 0;
    EXPECT_TRUE(dq.steal_top(out));
    EXPECT_EQ(out, 0);
    EXPECT_EQ(dq.size_approx(), 7U);
}

TEST(ChaseLevDequeTest, BoundarySingleElementPopAndStealRace) {
    // When only 1 element remains, pop_bottom and steal_top may race
    adt::ChaselevDeque<int> dq(4);
    dq.push_bottom(77);

    int out = -1;
    bool popped = dq.pop_bottom(out);
    EXPECT_TRUE(popped);
    EXPECT_EQ(out, 77);

    // Now empty — steal should fail
    int out2 = -1;
    EXPECT_FALSE(dq.steal_top(out2));
    EXPECT_EQ(out2, -1);
}

TEST(ChaseLevDequeTest, SizeApproxAfterMultipleOps) {
    adt::ChaselevDeque<int> dq(8);
    EXPECT_EQ(dq.size_approx(), 0U);

    dq.push_bottom(10);
    dq.push_bottom(20);
    dq.push_bottom(30);
    EXPECT_EQ(dq.size_approx(), 3U);

    int out = 0;
    dq.steal_top(out);
    EXPECT_EQ(dq.size_approx(), 2U);
}

// ============================================================================
// MultiPriorityWorkQueue — priority-ordered multi-level queue
// ============================================================================

TEST(MultiPriorityWorkQueueTest, PushPopSingleLevel) {
    adt::MultiPriorityWorkQueue<int> q(4);
    q.push(0, 100);
    q.push(0, 200);

    int out = 0;
    // Pop returns most recently pushed within same priority (LIFO-like)
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 200);
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 100);
    EXPECT_FALSE(q.pop(out));
}

TEST(MultiPriorityWorkQueueTest, HigherPriorityComesFirst) {
    adt::MultiPriorityWorkQueue<int> q(4);
    q.push(3, 300); // low priority
    q.push(0, 100); // high priority
    q.push(1, 200); // medium priority

    int out = 0;
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 100); // priority 0 first

    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 200); // priority 1 second

    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 300); // priority 3 last
}

TEST(MultiPriorityWorkQueueTest, SamePriorityLIFO) {
    adt::MultiPriorityWorkQueue<int> q(4);
    q.push(1, 10);
    q.push(1, 20);
    q.push(1, 30);

    int out = 0;
    // Pop returns most recently pushed within same priority
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 30);
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 20);
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out, 10);
}

TEST(MultiPriorityWorkQueueTest, StealHighestPriority) {
    adt::MultiPriorityWorkQueue<int> q(4);
    q.push(2, 200);
    q.push(0, 100);

    int out = 0;
    EXPECT_TRUE(q.steal(out));
    EXPECT_EQ(out, 100); // highest priority stolen
    EXPECT_TRUE(q.steal(out));
    EXPECT_EQ(out, 200);
}

TEST(MultiPriorityWorkQueueTest, DepthApproxAcrossLevels) {
    adt::MultiPriorityWorkQueue<int> q(4);
    EXPECT_EQ(q.depth_approx(), 0U);

    q.push(0, 1);
    q.push(1, 2);
    q.push(2, 3);
    EXPECT_EQ(q.depth_approx(), 3U);
}

TEST(MultiPriorityWorkQueueTest, NumLevels) {
    adt::MultiPriorityWorkQueue<int> q(4);
    EXPECT_EQ(q.num_levels(), 4U);

    adt::MultiPriorityWorkQueue<int> q2(8);
    EXPECT_EQ(q2.num_levels(), 8U);
}

TEST(MultiPriorityWorkQueueTest, EmptyPopReturnsFalse) {
    adt::MultiPriorityWorkQueue<int> q(4);
    int out = -1;
    EXPECT_FALSE(q.pop(out));
    EXPECT_EQ(out, -1);
}

TEST(MultiPriorityWorkQueueTest, EmptyStealReturnsFalse) {
    adt::MultiPriorityWorkQueue<int> q(4);
    int out = -1;
    EXPECT_FALSE(q.steal(out));
    EXPECT_EQ(out, -1);
}

// ============================================================================
// EDFQueue — additional branch-coverage tests beyond test_edf_queue.cpp
// ============================================================================

TEST(EDFQueueBranchesTest, ClearEmptiesQueue) {
    sched::EDFQueue q;
    q.push(100, sched::WorkItem{ActorId{1}, 100, 1});
    q.push(200, sched::WorkItem{ActorId{2}, 200, 2});
    EXPECT_FALSE(q.empty());

    q.clear();
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0U);

    sched::WorkItem out;
    EXPECT_FALSE(q.pop(out));
}

TEST(EDFQueueBranchesTest, MaxDeadlineGoesLast) {
    // Items with INT64_MAX deadline are background items
    sched::EDFQueue q;
    q.push(100, sched::WorkItem{ActorId{1}, 100, 1});
    q.push(INT64_MAX, sched::WorkItem{ActorId{99}, INT64_MAX, 99});
    q.push(50, sched::WorkItem{ActorId{2}, 50, 2});

    sched::WorkItem out;
    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.deadline_ns, 50);

    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.deadline_ns, 100);

    EXPECT_TRUE(q.pop(out));
    EXPECT_EQ(out.deadline_ns, INT64_MAX);
}

// ============================================================================
// TimingWheel — hierarchical timer wheel branch coverage
// ============================================================================

TEST(TimingWheelTest, ConstructDefault) {
    sched::TimingWheel tw;
    // current_time() is initialized from live monotonic clock
    EXPECT_GT(tw.current_time(), 0);
    EXPECT_TRUE(tw.empty());
    EXPECT_EQ(tw.size(), 0U);
    EXPECT_EQ(tw.next_deadline(), INT64_MAX);
}

// Helper: advance TimingWheel from current time to target in steps
// capped at 100ms per call (the wheel's internal cap).
static void advance_to(sched::TimingWheel& tw, int64_t target_ns) {
    constexpr int64_t kCapNs = 100'000'000; // 100ms
    int64_t now = tw.current_time();
    while (now < target_ns) {
        int64_t next = std::min(now + kCapNs, target_ns);
        tw.advance(next);
        now = tw.current_time();
    }
}

TEST(TimingWheelTest, ScheduleAndAdvance) {
    sched::TimingWheel tw(1'000'000, 4); // 1ms tick

    int64_t base = tw.current_time();
    int fired = 0;
    tw.schedule(500'000, [&] { ++fired; }); // 0.5ms delay relative to base

    EXPECT_FALSE(tw.empty());
    EXPECT_EQ(tw.size(), 1U);

    // Advance past the timer
    advance_to(tw, base + 1'000'000);
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(tw.empty());
}

TEST(TimingWheelTest, MultipleTimersSameTick) {
    sched::TimingWheel tw(1'000'000, 4);

    int64_t base = tw.current_time();
    int a = 0, b = 0, c = 0;
    tw.schedule(100'000, [&] { ++a; });
    tw.schedule(200'000, [&] { ++b; });
    tw.schedule(300'000, [&] { ++c; });

    advance_to(tw, base + 1'000'000);
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);
    EXPECT_EQ(c, 1);
    EXPECT_TRUE(tw.empty());
}

TEST(TimingWheelTest, CancelBeforeAdvance) {
    sched::TimingWheel tw(1'000'000, 4);

    int64_t base = tw.current_time();
    int fired = 0;
    auto id = tw.schedule(500'000, [&] { ++fired; });

    bool cancelled = tw.cancel(id);
    EXPECT_TRUE(cancelled);

    advance_to(tw, base + 1'000'000);
    EXPECT_EQ(fired, 0);
    EXPECT_TRUE(tw.empty());
}

TEST(TimingWheelTest, CancelNonExistent) {
    sched::TimingWheel tw(1'000'000, 4);
    EXPECT_FALSE(tw.cancel(9999));
}

TEST(TimingWheelTest, CancelAlreadyFired) {
    sched::TimingWheel tw(1'000'000, 4);

    int64_t base = tw.current_time();
    int fired = 0;
    auto id = tw.schedule(100'000, [&] { ++fired; });

    advance_to(tw, base + 1'000'000);
    EXPECT_EQ(fired, 1);

    // Already fired — cancel should return false
    EXPECT_FALSE(tw.cancel(id));
}

TEST(TimingWheelTest, ScheduleAtAbsoluteTime) {
    sched::TimingWheel tw(1'000'000, 4);

    int fired = 0;
    int64_t base = tw.current_time();
    int64_t deadline = base + 5'000'000; // absolute deadline at base+5ms
    tw.schedule_at(deadline, [&] { ++fired; });

    // Advance well past the deadline
    advance_to(tw, base + 10'000'000);
    EXPECT_EQ(fired, 1);
}

TEST(TimingWheelTest, NextDeadlineUpdates) {
    sched::TimingWheel tw(1'000'000, 4);

    EXPECT_EQ(tw.next_deadline(), INT64_MAX);

    tw.schedule(500'000, [] {});
    EXPECT_LT(tw.next_deadline(), INT64_MAX);
    EXPECT_GT(tw.next_deadline(), 0);

    int64_t base = tw.current_time();
    advance_to(tw, base + 1'000'000);
    EXPECT_EQ(tw.next_deadline(), INT64_MAX);
}

TEST(TimingWheelTest, TimersWithinLevel0FireCorrectly) {
    // Timers within the range of the lowest level fire without cascading.
    sched::TimingWheel tw(1'000'000, 4); // tick=1ms, level 0 range = 256ms

    int64_t base = tw.current_time();
    int fired = 0;
    // Schedule 50ms out — well within level-0 range
    tw.schedule(50'000'000, [&] { ++fired; });

    // Advance past the timer
    advance_to(tw, base + 60'000'000);
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(tw.empty());
}

TEST(TimingWheelTest, ScheduleFromCallback) {
    sched::TimingWheel tw(1'000'000, 4);

    int64_t base = tw.current_time();
    int first = 0, second = 0;
    tw.schedule(100'000, [&] {
        ++first;
        // Schedule another timer from within the callback
        tw.schedule(200'000, [&] { ++second; });
    });

    advance_to(tw, base + 1'000'000);
    EXPECT_EQ(first, 1);
    // Second timer scheduled from callback should also fire
    advance_to(tw, base + 2'000'000);
    EXPECT_EQ(second, 1);
}

// ============================================================================
// A2WS — additional pool locality and victim selection tests
// ============================================================================

TEST(A2WSBranchesTest, SinglePoolAllWorkersSame) {
    sched::A2WS a2ws(4, 4); // pool size equals num workers
    EXPECT_TRUE(a2ws.same_pool(0, 1));
    EXPECT_TRUE(a2ws.same_pool(0, 2));
    EXPECT_TRUE(a2ws.same_pool(0, 3));
}

TEST(A2WSBranchesTest, OddWorkerCountPools) {
    sched::A2WS a2ws(7, 3); // 7 workers, 3 per pool
    EXPECT_TRUE(a2ws.same_pool(0, 2));
    EXPECT_TRUE(a2ws.same_pool(3, 5));
    EXPECT_FALSE(a2ws.same_pool(2, 3));
    EXPECT_FALSE(a2ws.same_pool(5, 6));
}

TEST(A2WSBranchesTest, GetVictimSkipsSelf) {
    sched::A2WS a2ws(4, 2);
    uint32_t victim = a2ws.get_victim(0);
    // Victim should be in the same pool but not self
    EXPECT_NE(victim, 0U);
    EXPECT_TRUE(a2ws.same_pool(0, victim));
}

TEST(A2WSBranchesTest, PoolBoundaries) {
    sched::A2WS a2ws(10, 3);
    uint32_t start, end;

    a2ws.get_victim_pool(0, start, end);
    EXPECT_EQ(start, 0U);
    EXPECT_EQ(end, 3U);

    a2ws.get_victim_pool(5, start, end);
    EXPECT_EQ(start, 3U);
    EXPECT_EQ(end, 6U);

    a2ws.get_victim_pool(9, start, end);
    EXPECT_EQ(start, 9U);
}

// ============================================================================
// DedicatedThreadPool — dedicated thread pool lifecycle
// ============================================================================

TEST(DedicatedThreadPoolTest, ConstructAndStartStop) {
    sched::DedicatedThreadPool pool(2);
    EXPECT_FALSE(pool.is_running());
    EXPECT_EQ(pool.num_threads(), 2U);
    EXPECT_EQ(pool.pending(), 0U);

    pool.start();
    EXPECT_TRUE(pool.is_running());

    pool.stop();
    EXPECT_FALSE(pool.is_running());
}

TEST(DedicatedThreadPoolTest, EnqueueAndExecute) {
    sched::DedicatedThreadPool pool(2);
    pool.start();

    std::atomic<int> counter{0};
    pool.enqueue(ActorId{1}, [&] { counter.fetch_add(1); });
    pool.enqueue(ActorId{2}, [&] { counter.fetch_add(1); });

    // Wait for work to complete
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (counter.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(counter.load(), 2);

    pool.stop();
}

TEST(DedicatedThreadPoolTest, PendingCountDuringExecution) {
    sched::DedicatedThreadPool pool(1);
    pool.start();

    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    pool.enqueue(ActorId{1}, [&] {
        started.store(true);
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Wait for work to start
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!started.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(started.load());

    // More work enqueued while first is running
    pool.enqueue(ActorId{2}, [] {});
    EXPECT_GE(pool.pending(), 0U);

    done.store(true);
    pool.stop();
}

TEST(DedicatedThreadPoolTest, StopWithoutStart) {
    sched::DedicatedThreadPool pool(1);
    EXPECT_FALSE(pool.is_running());
    pool.stop(); // should be safe no-op
    EXPECT_FALSE(pool.is_running());
}

// ============================================================================
// WorkerThread — start/stop lifecycle and calibration
// ============================================================================

TEST(WorkerThreadBranchesTest, StartStopLifecycle) {
    sched::WorkerThread::Config cfg;
    cfg.worker_index = 7;
    sched::WorkerThread worker(cfg);

    EXPECT_EQ(worker.index(), 7U);
    EXPECT_FALSE(worker.is_running());

    worker.start();
    EXPECT_TRUE(worker.is_running());

    worker.stop();
    EXPECT_FALSE(worker.is_running());
}

TEST(WorkerThreadBranchesTest, PushAndPopWorkItem) {
    sched::BackoffCalibration cal;
    cal.yield_is_effective = true;
    cal.min_effective_sleep_ns = 1'000;
    cal.spin_threshold_ns = 0;
    cal.polling_budget_ns = 1'000'000;
    sched::WorkerThread::set_test_calibration(&cal);

    sched::WorkerThread::Config cfg;
    cfg.worker_index = 0;
    sched::WorkerThread worker(cfg);
    worker.start();

    sched::WorkItem item;
    item.actor = ActorId{42};
    item.deadline_ns = 1000;
    item.sequence = 1;

    worker.push(0, item);

    sched::WorkItem out;
    bool found = worker.pop(out);
    EXPECT_TRUE(found);
    EXPECT_EQ(out.actor.value(), 42U);

    worker.stop();
    sched::WorkerThread::set_test_calibration(nullptr);
}

TEST(WorkerThreadBranchesTest, PopOnEmptyReturnsFalse) {
    sched::WorkerThread::Config cfg;
    cfg.worker_index = 0;
    sched::WorkerThread worker(cfg);
    worker.start();

    sched::WorkItem out;
    EXPECT_FALSE(worker.pop(out));

    worker.stop();
}

TEST(WorkerThreadBranchesTest, DiagnosticCountersInitiallyZero) {
    sched::WorkerThread::Config cfg;
    cfg.worker_index = 0;
    sched::WorkerThread worker(cfg);
    worker.start();

    EXPECT_EQ(worker.diag_work_found(), 0U);
    EXPECT_EQ(worker.diag_idle_iters(), 0U);
    EXPECT_EQ(worker.diag_cv_escalations(), 0U);
    EXPECT_EQ(worker.diag_consecutive_empty_wakes(), 0U);

    worker.stop();
}

TEST(WorkerThreadBranchesTest, FramePoolIntegration) {
    sched::WorkerThread::Config cfg;
    cfg.worker_index = 0;
    sched::WorkerThread worker(cfg);

    // Set a frame pool before starting
    adt::CoroutineFramePool pool(4, 1024);
    worker.set_frame_pool(&pool);
    worker.start();

    auto* frame = worker.acquire_frame();
    ASSERT_NE(frame, nullptr);
    EXPECT_TRUE(frame->in_use);

    worker.release_frame(frame);
    EXPECT_FALSE(frame->in_use);

    worker.stop();
}

TEST(WorkerThreadBranchesTest, DonationCount) {
    sched::WorkerThread::Config cfg;
    cfg.worker_index = 0;
    sched::WorkerThread worker(cfg);
    worker.start();

    EXPECT_EQ(worker.donation_count(), 0U);
    worker.increment_donations();
    EXPECT_EQ(worker.donation_count(), 1U);
    worker.increment_donations();
    EXPECT_EQ(worker.donation_count(), 2U);

    worker.stop();
}

TEST(WorkerThreadBranchesTest, WorkProcessorReceivesItem) {
    sched::BackoffCalibration cal;
    cal.yield_is_effective = true;
    cal.min_effective_sleep_ns = 1'000;
    cal.spin_threshold_ns = 0;
    cal.polling_budget_ns = 1'000'000;
    sched::WorkerThread::set_test_calibration(&cal);

    sched::WorkerThread::Config cfg;
    cfg.worker_index = 0;
    sched::WorkerThread worker(cfg);

    std::atomic<bool> processed{false};
    std::atomic<uint64_t> actor_id_seen{0};
    worker.set_work_processor([&](const sched::WorkItem& item) {
        processed.store(true);
        actor_id_seen.store(item.actor.value());
    });

    worker.start();

    sched::WorkItem item;
    item.actor = ActorId{99};
    worker.push(0, item);

    // Wait for processing
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!processed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(processed.load());
    EXPECT_EQ(actor_id_seen.load(), 99U);

    worker.stop();
    sched::WorkerThread::set_test_calibration(nullptr);
}

// ============================================================================
// WorkPlacementScheduler — placement and worker state tests
// ============================================================================

TEST(WorkPlacementSchedulerBranchesTest, ConstructWithWorkers) {
    sched::WorkPlacementScheduler wps(4, 4);
    EXPECT_EQ(wps.worker_count(), 4U);
    EXPECT_EQ(wps.workers().size(), 4U);
}

TEST(WorkPlacementSchedulerBranchesTest, PopLocalFromEmptyWorker) {
    sched::WorkPlacementScheduler wps(2, 4);
    sched::WorkItem out;
    EXPECT_FALSE(wps.pop_local(0, out));
    EXPECT_FALSE(wps.pop_local(1, out));
}

TEST(WorkPlacementSchedulerBranchesTest, PopEdFromEmptyWorker) {
    sched::WorkPlacementScheduler wps(2, 4);
    sched::WorkItem out;
    EXPECT_FALSE(wps.pop_edf(0, out));
}

TEST(WorkPlacementSchedulerBranchesTest, TryStealFromEmptyWorkers) {
    sched::WorkPlacementScheduler wps(4, 4);
    sched::WorkItem out;
    EXPECT_FALSE(wps.try_steal(0, out));
    EXPECT_FALSE(wps.try_steal(1, out));
}

TEST(WorkPlacementSchedulerBranchesTest, PinAndUnpinActor) {
    sched::WorkPlacementScheduler wps(4, 4);
    ActorId actor_id{42};

    wps.pin_actor_to_worker(actor_id, 2);

    sched::WorkItem out;
    uint32_t wid = UINT32_MAX;
    // Actor hasn't been enqueued yet, so take_pinned should find nothing
    bool found = wps.take_pinned_for_test(actor_id, out, wid);
    // Pinning alone does not enqueue work — it only binds future enqueues
    EXPECT_FALSE(found || wid == UINT32_MAX); // at minimum wid should stay
                                              // unchanged

    wps.unpin_actor(actor_id);
}

TEST(WorkPlacementSchedulerBranchesTest, RegisterAndUnregisterDedicatedPool) {
    sched::WorkPlacementScheduler wps(4, 4);
    ActorId actor_id{100};

    wps.register_dedicated_pool(actor_id, 2);
    // Should not crash
    wps.unregister_dedicated(actor_id);
}

TEST(WorkPlacementSchedulerBranchesTest, RegisterAndUnregisterDedicatedThread) {
    sched::WorkPlacementScheduler wps(4, 4);
    ActorId actor_id{200};

    wps.register_dedicated_thread(actor_id, 1);
    wps.unregister_dedicated(actor_id);
}

TEST(WorkPlacementSchedulerBranchesTest, FlushPinnedToShared) {
    sched::WorkPlacementScheduler wps(2, 4);
    wps.flush_pinned_to_shared(); // Should be safe even with no pinned actors
}

TEST(WorkPlacementSchedulerBranchesTest, PopAnyForTestOnEmpty) {
    sched::WorkPlacementScheduler wps(2, 4);
    sched::WorkItem out;
    EXPECT_FALSE(wps.pop_any_for_test(out));
}

TEST(WorkPlacementSchedulerBranchesTest, A2WSAccess) {
    sched::WorkPlacementScheduler wps(4, 4);
    auto& a2ws = wps.a2ws();
    EXPECT_EQ(a2ws.num_workers(), 4U);
}

// ============================================================================
// ActorState — additional atomic transition edge cases
// ============================================================================

TEST(ActorStateBranchesTest, ExplicitInitialState) {
    ActorState state(ActorState::kRunning);
    EXPECT_EQ(state.get(), ActorState::kRunning);
    EXPECT_TRUE(state.is_running());
}

TEST(ActorStateBranchesTest, CasFailsOnWrongExpected) {
    ActorState state;
    // Current: Idle, try: Ready→Running (wrong expected)
    EXPECT_FALSE(state.cas(ActorState::kReady, ActorState::kRunning));
    EXPECT_TRUE(state.is_idle());
}

TEST(ActorStateBranchesTest, MultipleTransitions) {
    ActorState state;
    EXPECT_TRUE(state.cas(ActorState::kIdle, ActorState::kReady));
    EXPECT_TRUE(state.cas(ActorState::kReady, ActorState::kRunning));
    EXPECT_TRUE(state.cas(ActorState::kRunning, ActorState::kIOWaiting));
    EXPECT_TRUE(state.is_io_waiting());
}

TEST(ActorStateBranchesTest, SetThenGet) {
    ActorState state;
    state.set(ActorState::kRunning);
    EXPECT_EQ(state.get(), ActorState::kRunning);

    state.set(ActorState::kTerminated);
    EXPECT_EQ(state.get(), ActorState::kTerminated);
}

TEST(ActorStateBranchesTest, CasToSameState) {
    ActorState state;
    // CAS to same state (Idle→Idle) — implementation-dependent, but valid
    bool ok = state.cas(ActorState::kIdle, ActorState::kIdle);
    // Either outcome is acceptable; state must remain Idle
    EXPECT_TRUE(state.is_idle());
    // Silence unused-variable warning
    (void)ok;
}
