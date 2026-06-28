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

// System test: TimerPlane end-to-end
// Validates full-stack timer scheduling, delivery, and graceful shutdown
// with pending timers.

#include <hpactor/actor/actor_system.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace hpactor {
namespace {

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: End-to-end schedule and fire multiple timers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TimerPlaneSystem, EndToEndScheduleAndFire) {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    std::atomic<int> count{0};
    auto cb = [&count]() { count.fetch_add(1); };

    [[maybe_unused]] auto h1 =
        system.scheduler()->schedule_after(cb, 1'000'000LL); // 1ms
    [[maybe_unused]] auto h2 =
        system.scheduler()->schedule_after(cb, 2'000'000LL); // 2ms

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (count.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(count.load(), 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Graceful shutdown with far-future pending timers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TimerPlaneSystem, GracefulShutdownWithPendingTimers) {
    // Schedule timers, then destroy the system.  Should not crash.
    {
        Config config;
        config.scheduler_threads = 0;
        ActorSystem sys(config);

        [[maybe_unused]] auto h =
            sys.scheduler()->schedule_after([]() {}, 60'000'000'000LL); // far
                                                                        // future
                                                                        // (60s)
        // System destroyed at end of scope — should handle gracefully
    }
    SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Scheduler accessible and schedule_every works
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TimerPlaneSystem, SchedulerTimerApiAccessible) {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    // Verify scheduler() returns a valid pointer.
    auto* sched = system.scheduler();
    ASSERT_NE(sched, nullptr);

    // Schedule recurring timers and verify they fire.
    // schedule_every does not call notify_one(), so the timer thread may
    // sleep up to 100ms before noticing the new timer.
    std::atomic<int> tick_count{0};
    auto handle =
        sched->schedule_every([&tick_count]() { tick_count.fetch_add(1); },
                              5'000'000LL); // 5ms interval

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (tick_count.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sched->cancel_timer(handle);

    EXPECT_GE(tick_count.load(), 1);

    // Verify worker_count and is_running are functional.
    EXPECT_EQ(sched->worker_count(), 0);
    EXPECT_TRUE(sched->is_running());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Cancel prevents delivery at system level
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TimerPlaneSystem, CancelPreventsDelivery) {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    std::atomic<bool> fired{false};

    // Pause timer advancement before scheduling to eliminate a race where
    // the timer thread's current_time_ is stale (up to 100 ms from its last
    // advance cycle).  schedule_after() computes the deadline relative to
    // that cached current_time_, so a 5 ms delay can land in the past and
    // fire on the very next advance — before cancel_timer() can remove it.
    // Pausing prevents any advance during the schedule→cancel window.
    system.scheduler()->pause_workers();

    auto handle =
        system.scheduler()->schedule_after([&fired]() { fired.store(true); },
                                           5'000'000LL); // 5ms
    system.scheduler()->cancel_timer(handle);

    system.scheduler()->resume_workers();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fired.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(fired.load());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: Cancel invalid handle does not crash
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TimerPlaneSystem, CancelInvalidHandleIsSafe) {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    // Cancelling invalid handles should not crash or throw.
    system.scheduler()->cancel_timer(sched::TimerHandle{});
    system.scheduler()->cancel_timer(sched::TimerHandle{999999});
    SUCCEED();
}

} // namespace
} // namespace hpactor
