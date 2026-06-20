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

// System test: Cross-Subsystem Edge Cases
// Validates actor spawn during shutdown, message send to terminated actor,
// zero/large scheduled delays, concurrent schedules, empty behavior, long
// names, max workers, shutdown with pending timers/schedules, double shutdown,
// and spawn-before-init edge cases.

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>

#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using namespace hpactor;

using CountingActor = test::CountingActor;
using EchoActor = test::EchoActor;
using ForwardingActor = test::ForwardingActor;
using FailingActor = test::FailingActor;

HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);
HPACTOR_REGISTER_ACTOR("EchoActor", EchoActor);
HPACTOR_REGISTER_ACTOR("ForwardingActor", ForwardingActor);
HPACTOR_REGISTER_ACTOR("FailingActor", FailingActor);

// ── Actor with no handler behavior ───────────────────────────────────────────

class EmptyBehaviorActor : public EventBasedActor, public LifecycleActor {
  public:
    int handler_count = 0;

    EmptyBehaviorActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(Behavior{});
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    Behavior make_behavior() override {
        return Behavior{};
    }
};

HPACTOR_REGISTER_ACTOR("EmptyBehaviorActor", EmptyBehaviorActor);

// ── Actor that schedules messages to self ────────────────────────────────────

class SchedulingActor : public EventBasedActor, public LifecycleActor {
  public:
    std::vector<uint32_t> received_tags;
    int schedule_count = 0;

    SchedulingActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            received_tags.push_back(static_cast<uint32_t>(msg.type_id()));
            schedule_count++;
        }};
    }
};

HPACTOR_REGISTER_ACTOR("SchedulingActor", SchedulingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Actor spawn during system shutdown
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, SpawnDuringShutdown) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{100};
    opts.actor_drain_timeout = std::chrono::milliseconds{100};
    opts.cluster_leave_timeout = std::chrono::milliseconds{100};
    opts.force_after_timeout = true;

    // Shutdown the system in a separate thread so we don't block
    std::atomic<bool> shutdown_done{false};
    std::thread t([&]() {
        auto result = system.shutdown(opts);
        (void)result;
        shutdown_done = true;
    });

    // Wait for shutdown to start
    while (system.is_running() && !shutdown_done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Spawn after shutdown started — may fail gracefully or succeed depending
    // on timing. The key is that it doesn't crash.
    auto actor = system.spawn<CountingActor>();
    // Either we got a valid actor or the system was already stopped.
    // Both outcomes are acceptable — no crash is the invariant.

    t.join();
    EXPECT_TRUE(shutdown_done);
    EXPECT_EQ(system.shutdown_phase(), ShutdownPhase::Stopped);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Message send to non-existent / terminated actor
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, MessageSendToNonexistentActor) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    ActorId nonexistent(99999);
    TypedMessage msg(TypeTag(0x1001), StreamBuffer{});

    auto result = system.try_deliver_local(nonexistent, std::move(msg));
    EXPECT_EQ(result.code, mailbox::EnqueueResultCode::ActorNotFound);

    // Check DLQ got the record
    auto snap = system.dead_letter_snapshot();
    EXPECT_GE(snap.depth, 1u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Schedule with zero delay
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, ScheduleWithZeroDelay) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto actor = system.spawn<SchedulingActor>();
    auto* raw = static_cast<SchedulingActor*>(actor.get().get());

    // Schedule a message with zero delay
    TypedMessage msg(TypeTag(0x1001), StreamBuffer{});
    auto handle =
        raw->context()->schedule(std::chrono::milliseconds(0), std::move(msg));
    EXPECT_NE(handle, AlarmHandle{0});

    // The zero-delay message should be delivered by the live scheduler
    bool delivered =
        test::assert_eventually([&]() { return raw->schedule_count >= 1; }, 2000);
    EXPECT_TRUE(delivered);
    EXPECT_GE(raw->schedule_count, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Schedule with very large delay
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, ScheduleWithVeryLargeDelay) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto actor = system.spawn<SchedulingActor>();
    auto* raw = static_cast<SchedulingActor*>(actor.get().get());

    // Schedule with a very large delay (10 minutes)
    TypedMessage msg(TypeTag(0x2001), StreamBuffer{});
    auto handle =
        raw->context()->schedule(std::chrono::minutes(10), std::move(msg));
    EXPECT_NE(handle, AlarmHandle{0});

    // Cancel it — the message should not arrive
    raw->context()->cancel_schedule(handle);
    // After cancellation, the message should not be delivered
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(raw->schedule_count, 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: Multiple concurrent scheduled messages
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, MultipleConcurrentScheduledMessages) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto actor = system.spawn<SchedulingActor>();
    auto* raw = static_cast<SchedulingActor*>(actor.get().get());
    auto* ctx = raw->context();

    constexpr int kNumScheduled = 5;
    for (int i = 0; i < kNumScheduled; ++i) {
        TypedMessage msg(TypeTag(static_cast<uint32_t>(0x3001 + i)),
                         StreamBuffer{});
        auto handle = ctx->schedule(std::chrono::milliseconds(0), std::move(msg));
        EXPECT_NE(handle, AlarmHandle{0});
    }

    // All scheduled messages should be delivered by the live scheduler
    bool delivered = test::assert_eventually(
        [&]() { return raw->schedule_count >= kNumScheduled; }, 2000);
    EXPECT_TRUE(delivered);
    EXPECT_GE(raw->schedule_count, kNumScheduled);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 6: Actor with empty behavior receives messages but handles none
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, ActorWithEmptyBehavior) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    auto actor = system.spawn<EmptyBehaviorActor>();
    auto* raw = static_cast<EmptyBehaviorActor*>(actor.get().get());

    // Send a message to the actor — should be enqueued and possibly processed
    // but the empty behavior won't increment handler_count
    TypedMessage msg(TypeTag(0x4001), StreamBuffer{});
    system.deliver_local(actor.id(), std::move(msg));

    // The handler_count should remain 0 since the behavior has no handler
    EXPECT_EQ(raw->handler_count, 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 7: Actor spawn with long name
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, ActorSpawnWithLongName) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    std::string long_name(256, 'x');
    system.register_actor(long_name, Actor{actor.get()});

    auto resolved = system.resolve_actor(long_name);
    EXPECT_TRUE(resolved.get() != nullptr);

    // Clean up
    system.unregister_actor(long_name);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 8: Scheduler with max workers — system still functional
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, SchedulerWithMaxWorkers) {
    Config cfg = test::config_with_scheduler(8);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* raw = static_cast<CountingActor*>(actor.get().get());

    TypedMessage msg(TypeTag(0x5001), StreamBuffer{});
    system.deliver_local(actor.id(), std::move(msg));

    hpactor::test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() { return raw->handler_count >= 1; });
    EXPECT_TRUE(done);
    EXPECT_GE(raw->handler_count, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 9: System shutdown with pending scheduled messages
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, ShutdownWithPendingScheduledMessages) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto actor = system.spawn<SchedulingActor>();
    auto* raw_sched = static_cast<SchedulingActor*>(actor.get().get());

    // Schedule a message with a long delay so it's pending at shutdown
    TypedMessage msg(TypeTag(0x6001), StreamBuffer{});
    auto handle =
        raw_sched->context()->schedule(std::chrono::seconds(60), std::move(msg));
    EXPECT_NE(handle, AlarmHandle{0});

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{200};
    opts.actor_drain_timeout = std::chrono::milliseconds{200};
    opts.cluster_leave_timeout = std::chrono::milliseconds{200};
    opts.force_after_timeout = true;

    auto result = system.shutdown(opts);
    EXPECT_TRUE(result.has_value());
    // Shutdown with a pending long-delay scheduled message may force-stop
    auto phase = system.shutdown_phase();
    EXPECT_TRUE(phase == ShutdownPhase::Stopped ||
                phase == ShutdownPhase::ForcedStop);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 10: Double shutdown attempt
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, DoubleShutdownAttempt) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{100};
    opts.actor_drain_timeout = std::chrono::milliseconds{100};
    opts.cluster_leave_timeout = std::chrono::milliseconds{100};
    opts.force_after_timeout = true;

    auto result1 = system.shutdown(opts);
    EXPECT_TRUE(result1.has_value());

    // Second shutdown on an already-stopped system should be safe
    auto result2 = system.shutdown(opts);
    EXPECT_TRUE(result2.has_value()); // Should still succeed, or report already
                                      // stopped

    EXPECT_EQ(system.shutdown_phase(), ShutdownPhase::Stopped);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 11: System shutdown with pending timers (EventLoop timers)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, ShutdownWithPendingEventLoopTimers) {
    Config cfg = test::config_with_scheduler(0);
    cfg.enable_network = true;
    ActorSystem system(cfg);

    // Register a timer on the event loop that would fire after shutdown
    auto* loop = system.event_loop();
    ASSERT_NE(loop, nullptr);

    std::atomic<bool> timer_fired{false};
    uint64_t handle =
        loop->run_after([&timer_fired]() { timer_fired = true; }, 5000);
    ASSERT_GT(handle, 0u);

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{100};
    opts.actor_drain_timeout = std::chrono::milliseconds{100};
    opts.cluster_leave_timeout = std::chrono::milliseconds{100};
    opts.force_after_timeout = true;

    auto result = system.shutdown(opts);
    // Shutdown should complete even with pending timers
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(system.shutdown_phase(), ShutdownPhase::Stopped);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 12: Spawn and message delivery before full SystemInit
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, SpawnAndDeliverBeforeSystemInit) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    // Spawn an actor and deliver a message — all before any explicit init
    auto actor = system.spawn<CountingActor>();

    TypedMessage msg(TypeTag(0x7001), StreamBuffer{});
    system.deliver_local(actor.id(), std::move(msg));

    // After system construction, is_ready() should already be true
    // (ready_ starts true in the constructor and system-init is for
    // topology bootstrap)
    EXPECT_TRUE(system.is_ready());
    EXPECT_TRUE(system.is_running());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 13: Actor spawn with repeated same name (re-registration)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemEdgeCases, ActorReRegisterSameName) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    auto a1 = system.spawn<CountingActor>();
    system.register_actor("duplicate", Actor{a1.get()});

    // Re-register with a different actor under same name
    auto a2 = system.spawn<CountingActor>();
    system.register_actor("duplicate", Actor{a2.get()});

    auto resolved = system.resolve_actor("duplicate");
    EXPECT_TRUE(resolved.get() != nullptr);

    system.unregister_actor("duplicate");
}
