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

// System test: Mailbox & Scheduler Workflow
// Validates bounded mailbox overflow → DLQ → priority lanes → pressure state
// transitions → scheduler dispatch → work stealing → EDF → timers

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/mailbox/multi_lane_queue.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/sched/scheduler.hpp>

#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

using namespace hpactor;

using CountingActor = test::CountingActor;
using EchoActor = test::EchoActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);
HPACTOR_REGISTER_ACTOR("EchoActor", EchoActor);

// ── Helper: create a TypedMessage with a specific type tag ─────────────────

static TypedMessage make_msg(uint32_t type_val) {
    TypedMessage msg(TypeTag(type_val), StreamBuffer{});
    msg.set_sender_address(ActorAddress{});
    return msg;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Group 1: Mailbox Operations
// ═══════════════════════════════════════════════════════════════════════════════

// ── Test 1.1: Bounded mailbox overflow → DLQ routing ────────────────────────

TEST(MailboxScheduler, BoundedMailboxOverflowAndDlqRouting) {
    Config cfg = test::minimal_config();
    // Set a small bounded mailbox with DeadLetter overflow policy
    cfg.mailbox.default_capacity = 3;
    cfg.mailbox.default_policy = mailbox::OverflowPolicy::DeadLetter;
    // Reduce watermarks so we see pressure before the hard limit
    cfg.mailbox.high_watermark = 0.60;
    cfg.mailbox.critical_watermark = 0.95;

    ActorSystem system(cfg);

    auto a = system.spawn<CountingActor>();
    ActorId target = a.id();

    // Send 3 messages — should all be accepted (at capacity)
    for (uint32_t i = 0; i < 3; ++i) {
        auto result = system.try_deliver_local(target, make_msg(0x1000 + i),
                                               /*priority=*/0);
        EXPECT_TRUE(result.accepted()) << "msg " << i;
    }

    // 4th message should be routed to DLQ (overflow policy = DeadLetter)
    auto result = system.try_deliver_local(target, make_msg(0x2000),
                                           /*priority=*/0);
    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.code, mailbox::EnqueueResultCode::ReroutedToDeadLetter);

    // DLQ should contain the overflow record
    auto snap = system.dead_letter_snapshot();
    EXPECT_GE(snap.depth, 1);

    // Pop and verify the DLQ record
    mailbox::DeadLetterRecord rec;
    bool ok = system.pop_dead_letter(rec);
    EXPECT_TRUE(ok);
    EXPECT_EQ(rec.reason, mailbox::DeadLetterReason::OverflowPolicy);
}

// ── Test 1.2: MultiLaneQueue priority lane routing ──────────────────────────

TEST(MailboxScheduler, MultiLaneQueuePriorityLaneRouting) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    // Enable priority-aware routing with 4 lanes
    cfg.mailbox.priority_aware = true;
    cfg.mailbox.priority_levels = 4;

    ActorSystem system(cfg);

    auto a = system.spawn<CountingActor>();
    auto* raw = static_cast<CountingActor*>(a.get().get());
    ActorId target = a.id();

    // Send messages at different priorities BEFORE draining.
    // Lower priority number = higher priority lane (0 is highest).
    // Messages: p3 (lowest), p1, p2, p0 (highest)
    // Expected processing order: p0 (lane 0), p1 (lane 1), p2 (lane 2), p3
    // (lane 3)
    system.deliver_local(target, make_msg(0x1003), /*priority=*/3, INT64_MAX);
    system.deliver_local(target, make_msg(0x1001), /*priority=*/1, INT64_MAX);
    system.deliver_local(target, make_msg(0x1002), /*priority=*/2, INT64_MAX);
    system.deliver_local(target, make_msg(0x1000), /*priority=*/0, INT64_MAX);

    // Verify per-lane depths before draining
    auto* mbox = system.get_mailbox(target);
    ASSERT_NE(mbox, nullptr);
    auto snap = mbox->snapshot();
    EXPECT_EQ(snap.depth, 4u);
    // With 4 lanes, each should have 1 message
    EXPECT_EQ(snap.lane_depths[0], 1u); // p0
    EXPECT_EQ(snap.lane_depths[1], 1u); // p1
    EXPECT_EQ(snap.lane_depths[2], 1u); // p2
    EXPECT_EQ(snap.lane_depths[3], 1u); // p3

    // Drain deterministically
    test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() { return raw->handler_count >= 4; });
    EXPECT_TRUE(done);

    // Priority order: lane 0 first, lane 3 last
    EXPECT_EQ(raw->handler_count, 4);
    ASSERT_EQ(raw->received_type_ids.size(), 4u);
    EXPECT_EQ(raw->received_type_ids[0], 0x1000u); // highest priority
    EXPECT_EQ(raw->received_type_ids[1], 0x1001u);
    EXPECT_EQ(raw->received_type_ids[2], 0x1002u);
    EXPECT_EQ(raw->received_type_ids[3], 0x1003u); // lowest priority
}

// ── Test 1.3: Mailbox pressure state transitions ────────────────────────────

TEST(MailboxScheduler, MailboxPressureStateTransitions) {
    Config cfg = test::minimal_config();
    // Small capacity so we can trivially hit all watermarks
    cfg.mailbox.default_capacity = 10;
    cfg.mailbox.high_watermark = 0.30;     // SoftPressure at depth 3
    cfg.mailbox.low_watermark = 0.15;      // recover below depth 1.5 => depth 1
    cfg.mailbox.critical_watermark = 0.80; // HardPressure at depth 8
    cfg.mailbox.default_policy = mailbox::OverflowPolicy::RejectNewest;

    ActorSystem system(cfg);

    auto a = system.spawn<CountingActor>();
    ActorId target = a.id();
    auto* mbox = system.get_mailbox(target);
    ASSERT_NE(mbox, nullptr);

    // Initially Normal
    {
        auto snap = mbox->snapshot();
        EXPECT_EQ(snap.pressure_state, std::string("normal"));
    }

    // Fill to depth 3 → SoftPressure (ratio 3/10 = 0.30 >= high_watermark)
    for (uint32_t i = 0; i < 3; ++i) {
        auto r = system.try_deliver_local(target, make_msg(0x1000 + i));
        EXPECT_TRUE(r.accepted());
    }
    {
        auto snap = mbox->snapshot();
        EXPECT_EQ(snap.pressure_state, std::string("soft_pressure"));
    }

    // Fill to depth 8 → HardPressure (ratio 8/10 = 0.80 >= critical)
    for (uint32_t i = 0; i < 5; ++i) {
        auto r = system.try_deliver_local(target, make_msg(0x2000 + i));
        EXPECT_TRUE(r.accepted());
    }
    {
        auto snap = mbox->snapshot();
        EXPECT_EQ(snap.pressure_state, std::string("hard_pressure"));
        EXPECT_EQ(snap.depth, 8u);
    }

    // Fill to capacity 10 → reject (RejectNewest policy)
    for (uint32_t i = 0; i < 2; ++i) {
        auto r = system.try_deliver_local(target, make_msg(0x3000 + i));
        EXPECT_TRUE(r.accepted());
    }
    {
        auto snap = mbox->snapshot();
        EXPECT_EQ(snap.depth, 10u);
    }
    auto r = system.try_deliver_local(target, make_msg(0x4000));
    EXPECT_FALSE(r.accepted());
    EXPECT_EQ(r.code, mailbox::EnqueueResultCode::Rejected);
}

// ── Test 1.4: Backpressure signal emission through RejectNewest ─────────────

TEST(MailboxScheduler, BackpressureSignalOnRejection) {
    Config cfg = test::minimal_config();
    cfg.mailbox.default_capacity = 2;
    cfg.mailbox.high_watermark = 0.50;
    cfg.mailbox.critical_watermark = 1.00;
    cfg.mailbox.default_policy = mailbox::OverflowPolicy::RejectNewest;
    cfg.mailbox.backpressure_mode = mailbox::BackpressureMode::LocalSignal;

    ActorSystem system(cfg);

    auto a = system.spawn<CountingActor>();
    ActorId target = a.id();
    auto* mbox = system.get_mailbox(target);
    ASSERT_NE(mbox, nullptr);

    // Fill to capacity
    EXPECT_TRUE(system.try_deliver_local(target, make_msg(0x1001)).accepted());
    EXPECT_TRUE(system.try_deliver_local(target, make_msg(0x1002)).accepted());

    // This should reject with backpressure
    auto result = system.try_deliver_local(target, make_msg(0x2000));
    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.code, mailbox::EnqueueResultCode::Rejected);

    // The result carries pressure state information
    EXPECT_EQ(result.pressure_state, mailbox::MailboxPressureState::HardPressure);
    EXPECT_EQ(result.depth, 2u);
    EXPECT_EQ(result.capacity, 2u);

    // Mailbox snapshot reflects hard pressure
    auto snap = mbox->snapshot();
    EXPECT_EQ(snap.pressure_state, std::string("hard_pressure"));
    EXPECT_GT(snap.total_rejected, 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Group 2: Scheduler Operations
// ═══════════════════════════════════════════════════════════════════════════════

// ── Test 2.1: Scheduler worker dispatch and execution ───────────────────────

TEST(MailboxScheduler, SchedulerWorkerDispatchAndExecution) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;

    ActorSystem system(cfg);
    ASSERT_NE(system.scheduler(), nullptr);
    EXPECT_EQ(system.scheduler()->worker_count(), 1u);

    auto a = system.spawn<CountingActor>();
    auto* raw = static_cast<CountingActor*>(a.get().get());

    // Deliver 3 messages
    system.deliver_local(a.id(), make_msg(0x1001));
    system.deliver_local(a.id(), make_msg(0x1002));
    system.deliver_local(a.id(), make_msg(0x1003));

    // Verify mailbox has messages before drain
    auto* mbox = system.get_mailbox(a.id());
    ASSERT_NE(mbox, nullptr);
    EXPECT_FALSE(mbox->empty());

    // Drain all 3
    test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() { return raw->handler_count >= 3; });
    EXPECT_TRUE(done);

    EXPECT_EQ(raw->handler_count, 3);
    EXPECT_EQ(raw->received_type_ids.size(), 3u);
    EXPECT_TRUE(mbox->empty());
}

// ── Test 2.2: Work stealing between workers ─────────────────────────────────

TEST(MailboxScheduler, WorkStealingBetweenWorkers) {
    Config cfg = test::config_with_scheduler(2);
    // Start paused so we can deterministically set up both actors and deliver
    // all messages before workers begin processing.  This eliminates the race
    // between deliver_local() → notify_ready() (which drops AlreadyRunning
    // notifications) and the adaptive batch's mailbox-empty check, which
    // otherwise orphans messages under coverage-instrumentation slowdown.
    cfg.scheduler_start_paused = true;

    ActorSystem system(cfg);
    ASSERT_NE(system.scheduler(), nullptr);
    ASSERT_GE(system.scheduler()->worker_count(), 2u);

    auto a1 = system.spawn<CountingActor>();
    auto a2 = system.spawn<CountingActor>();
    auto* raw1 = static_cast<CountingActor*>(a1.get().get());
    auto* raw2 = static_cast<CountingActor*>(a2.get().get());

    // Deliver all messages to both actors while workers are paused.
    for (uint32_t i = 0; i < 5; ++i) {
        system.deliver_local(a1.id(), make_msg(0x1000 + i));
        system.deliver_local(a2.id(), make_msg(0x2000 + i));
    }

    // Resume workers — both actors have full mailboxes.  Workers compete for
    // work and may steal across deques, exercising the work-stealing path.
    system.scheduler()->resume_workers();

    // Wait for both actors to process all messages
    bool done = test::assert_eventually(
        [&]() { return raw1->handler_count >= 5 && raw2->handler_count >= 5; });
    EXPECT_TRUE(done);

    EXPECT_GE(raw1->handler_count, 5);
    EXPECT_GE(raw2->handler_count, 5);

    // Workers are running and processed messages — verify snapshots are
    // accessible
    auto snapshots = system.scheduler()->worker_snapshots();
    ASSERT_GE(snapshots.size(), 2u);
    // Worker snapshots contain structural fields like worker_index and
    // idle_model
    for (const auto& ws : snapshots) {
        EXPECT_LT(ws.worker_index, 100u); // valid worker index
    }
}

// ── Test 2.3: EDF (Earliest Deadline First) scheduling ──────────────────────

TEST(MailboxScheduler, EdfEarliestDeadlineFirstScheduling) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;

    ActorSystem system(cfg);

    auto a = system.spawn<CountingActor>();
    auto* raw = static_cast<CountingActor*>(a.get().get());

    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();

    // Deliver messages with EDF scheduling — verify delivery works
    system.deliver_local_edf(a.id(), make_msg(0x1001),
                             /*deadline_ns=*/now + 100'000'000LL, /*priority=*/0);
    system.deliver_local_edf(a.id(), make_msg(0x2002),
                             /*deadline_ns=*/now + 200'000'000LL, /*priority=*/0);

    // Drain deterministically
    test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() { return raw->handler_count >= 2; });
    EXPECT_TRUE(done);

    EXPECT_EQ(raw->handler_count, 2);
    ASSERT_EQ(raw->received_type_ids.size(), 2u);
    // Both messages were delivered — EDF delivery path works
    // (EDF intra-mailbox ordering is not guaranteed for a single actor —
    //  EDF at the scheduler level controls actor dispatch order, while
    //  the mailbox priority lanes control per-actor processing order.)
}

// ── Test 2.4: Timer scheduling and cancellation ─────────────────────────────

TEST(MailboxScheduler, TimerSchedulingAndCancellation) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = false; // Workers must run for timers to fire

    ActorSystem system(cfg);
    auto* sched = system.scheduler();
    ASSERT_NE(sched, nullptr);

    std::atomic<int> fire_count{0};

    // Schedule a timer that fires after a short delay
    auto handle = sched->schedule_after(
        [&]() { fire_count.fetch_add(1, std::memory_order_relaxed); },
        10'000'000LL); // 10 ms in ns

    EXPECT_NE(handle.value(), 0u);

    // Wait for the timer to fire
    bool fired =
        test::assert_eventually([&]() { return fire_count.load() >= 1; }, 2000);
    EXPECT_TRUE(fired);
    EXPECT_EQ(fire_count.load(), 1);

    // Schedule a timer and then cancel it before it fires
    std::atomic<int> cancel_fire{0};
    auto handle2 = sched->schedule_after(
        [&]() { cancel_fire.fetch_add(1, std::memory_order_relaxed); },
        500'000'000LL); // 500 ms — plenty of time to cancel

    sched->cancel_timer(handle2);

    // Give a small window for any spurious fire, but it should not fire
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(cancel_fire.load(), 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Group 3: Mailbox + Scheduler Integration
// ═══════════════════════════════════════════════════════════════════════════════

// ── Test 3.1: Actor message flow through mailbox → scheduler → actor ────────

TEST(MailboxScheduler, ActorMessageFlowThroughMailboxSchedulerActor) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;

    ActorSystem system(cfg);

    auto a = system.spawn<CountingActor>();
    auto* raw = static_cast<CountingActor*>(a.get().get());
    auto* mbox = system.get_mailbox(a.id());
    ASSERT_NE(mbox, nullptr);

    // 1. Before delivery: mailbox empty
    EXPECT_TRUE(mbox->empty());

    // 2. Deliver message through the pipeline
    TypedMessage msg = make_msg(0x1001);
    msg.set_sender_address(ActorAddress{});
    system.deliver_local(a.id(), std::move(msg));

    // 3. After delivery, before drain: mailbox non-empty
    EXPECT_FALSE(mbox->empty());

    // 4. Drain through scheduler — this calls the actor's handler
    test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() { return raw->handler_count >= 1; });
    EXPECT_TRUE(done);

    // 5. After drain: handler was invoked, mailbox is empty
    EXPECT_EQ(raw->handler_count, 1);
    EXPECT_EQ(raw->received_type_ids.size(), 1u);
    EXPECT_EQ(raw->received_type_ids[0], 0x1001u);
    EXPECT_TRUE(mbox->empty());
}

// ── Test 3.2: Multiple actors with priority messages ────────────────────────

TEST(MailboxScheduler, MultipleActorsWithPriorityMessages) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    cfg.mailbox.priority_aware = true;
    cfg.mailbox.priority_levels = 4;

    ActorSystem system(cfg);

    auto high_prio_actor = system.spawn<CountingActor>();
    auto low_prio_actor = system.spawn<CountingActor>();
    auto* high_raw = static_cast<CountingActor*>(high_prio_actor.get().get());
    auto* low_raw = static_cast<CountingActor*>(low_prio_actor.get().get());

    // Send high-priority (0) messages to high_prio_actor
    system.deliver_local(high_prio_actor.id(), make_msg(0x1001),
                         /*priority=*/0, INT64_MAX);

    // Send low-priority (3) messages to low_prio_actor
    system.deliver_local(low_prio_actor.id(), make_msg(0x2001),
                         /*priority=*/3, INT64_MAX);

    // Both mailboxes should have messages
    auto* mbox_high = system.get_mailbox(high_prio_actor.id());
    auto* mbox_low = system.get_mailbox(low_prio_actor.id());
    ASSERT_NE(mbox_high, nullptr);
    ASSERT_NE(mbox_low, nullptr);

    auto snap_high = mbox_high->snapshot();
    auto snap_low = mbox_low->snapshot();
    EXPECT_EQ(snap_high.depth, 1u);
    EXPECT_EQ(snap_low.depth, 1u);

    // Drain all
    test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() {
        return high_raw->handler_count >= 1 && low_raw->handler_count >= 1;
    });
    EXPECT_TRUE(done);

    EXPECT_EQ(high_raw->handler_count, 1);
    EXPECT_EQ(low_raw->handler_count, 1);
    EXPECT_EQ(high_raw->received_type_ids[0], 0x1001u);
    EXPECT_EQ(low_raw->received_type_ids[0], 0x2001u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Group 4: Edge Cases
// ═══════════════════════════════════════════════════════════════════════════════

// ── Test 4.1: Empty mailbox behavior ────────────────────────────────────────

TEST(MailboxScheduler, EmptyMailboxBehavior) {
    Config cfg = test::minimal_config(); // scheduler_threads = 0

    ActorSystem system(cfg);

    auto a = system.spawn<CountingActor>();
    auto* mbox = system.get_mailbox(a.id());
    ASSERT_NE(mbox, nullptr);

    // Fresh mailbox should be empty
    EXPECT_TRUE(mbox->empty());
    EXPECT_TRUE(mbox->was_empty());

    auto snap = mbox->snapshot();
    EXPECT_EQ(snap.depth, 0u);
    EXPECT_EQ(snap.total_enqueued, 0u);
    EXPECT_EQ(snap.total_dequeued, 0u);
    EXPECT_EQ(snap.total_rejected, 0u);
    EXPECT_EQ(snap.total_dropped, 0u);

    // EnqueueResults on empty target should not exist
    // (no delivery attempted yet to non-existent actors)
}

// ── Test 4.2: Scheduler with zero workers ───────────────────────────────────

TEST(MailboxScheduler, SchedulerWithZeroWorkers) {
    Config cfg = test::minimal_config(); // scheduler_threads = 0

    ActorSystem system(cfg);

    // With zero workers, the scheduler pointer may be null or present but idle
    // depending on implementation. The system should still be operational
    // for spawning actors.

    auto a = system.spawn<CountingActor>();
    EXPECT_NE(a.id().value(), 0u);

    // Can deliver messages even without workers
    auto result = system.try_deliver_local(a.id(), make_msg(0x1001));
    // Without a running scheduler, the message goes to mailbox but may not
    // be executed. The delivery itself should succeed.
    EXPECT_TRUE(result.accepted());

    auto* mbox = system.get_mailbox(a.id());
    ASSERT_NE(mbox, nullptr);
    EXPECT_FALSE(mbox->empty());
    EXPECT_EQ(mbox->snapshot().depth, 1u);

    // Scheduler may be present or nullptr depending on config
    // The important thing is the system doesn't crash
    EXPECT_TRUE(system.is_running());
}

// ── Test 4.3 (extra): Mailbox empty → non-empty → empty cycle ───────────────

TEST(MailboxScheduler, MailboxEmptyNonEmptyCycle) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;

    ActorSystem system(cfg);

    auto a = system.spawn<CountingActor>();
    auto* raw = static_cast<CountingActor*>(a.get().get());
    auto* mbox = system.get_mailbox(a.id());

    // Start empty
    EXPECT_TRUE(mbox->empty());

    // Send one message
    system.deliver_local(a.id(), make_msg(0x1001));
    EXPECT_FALSE(mbox->empty());

    // Drain
    test::SchedulerTestDriver driver(system);
    driver.drain_until([&]() { return raw->handler_count >= 1; });
    EXPECT_TRUE(mbox->empty());

    // Send another — should transition empty→non-empty again
    system.deliver_local(a.id(), make_msg(0x1002));
    EXPECT_FALSE(mbox->empty());

    driver.drain_until([&]() { return raw->handler_count >= 2; });
    EXPECT_TRUE(mbox->empty());
    EXPECT_EQ(raw->handler_count, 2);
}

// ── Test 4.4 (extra): Delivery to non-existent actor produces DLQ ────────────

TEST(MailboxScheduler, DeliveryToNonExistentActorDlq) {
    Config cfg = test::minimal_config();
    cfg.mailbox.default_policy = mailbox::OverflowPolicy::DeadLetter;

    ActorSystem system(cfg);

    // Clean any pre-existing DLQ records
    mailbox::DeadLetterRecord tmp;
    while (system.pop_dead_letter(tmp)) {
    }

    // Try to deliver to a non-existent actor
    TypedMessage msg = make_msg(0x9999);
    msg.set_sender_address(ActorAddress{});
    auto result = system.try_deliver_local(ActorId(99999), std::move(msg));

    EXPECT_EQ(result.code, mailbox::EnqueueResultCode::ActorNotFound);

    auto snap = system.dead_letter_snapshot();
    EXPECT_GE(snap.depth, 1);
}
