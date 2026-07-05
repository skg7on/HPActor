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

#include <hpactor/actor/system/actor_system.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace hpactor {
namespace {

TEST(TimerWakeupTest, ScheduleWakesSleepingTimerThread) {
    Config config;
    config.scheduler_threads = 0; // No worker threads needed
    ActorSystem system(config);

    std::atomic<bool> fired{false};

    // Schedule a far-future timer so the timer thread enters a long sleep.
    auto far = system.scheduler()->schedule_after([]() {}, 10'000'000'000LL); // 10 seconds
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Schedule a short timer.
    auto short_h = system.scheduler()->schedule_after(
        [&fired]() { fired.store(true); }, 1'000'000LL); // 1ms

    // Poll with generous timeout (5s per .claude/rules).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fired.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(fired.load()) << "Short timer should fire within generous timeout";

    // Clean up
    system.scheduler()->cancel_timer(far);
    if (!fired.load()) {
        system.scheduler()->cancel_timer(short_h);
    }
}

} // namespace
} // namespace hpactor
