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

class TimerPlaneIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config;
        config.scheduler_threads = 0; // deterministic
        system_ = std::make_unique<ActorSystem>(config);
    }
    void TearDown() override {
        system_.reset();
    }
    std::unique_ptr<ActorSystem> system_;
};

TEST_F(TimerPlaneIntegrationTest, ScheduleDeliversMessage) {
    std::atomic<bool> received{false};

    [[maybe_unused]] auto handle = system_->scheduler()->schedule_after(
        [&received]() { received.store(true); },
        1'000'000LL); // 1ms

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!received.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(received.load());
}

TEST_F(TimerPlaneIntegrationTest, CancelPreventsDelivery) {
    std::atomic<bool> fired{false};
    // Use a generous delay (500 ms) so the test thread always reaches
    // cancel_timer() before the timer thread fires the callback, even under
    // coverage-instrumentation or heavily contended CI runners.
    auto handle =
        system_->scheduler()->schedule_after([&fired]() { fired.store(true); },
                                             500'000'000LL); // 500ms
    system_->scheduler()->cancel_timer(handle);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fired.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(fired.load());
}

TEST_F(TimerPlaneIntegrationTest, MultipleTimersFire) {
    std::atomic<int> count{0};
    auto cb = [&count]() { count.fetch_add(1); };

    [[maybe_unused]] auto h1 =
        system_->scheduler()->schedule_after(cb, 1'000'000LL);
    [[maybe_unused]] auto h2 =
        system_->scheduler()->schedule_after(cb, 2'000'000LL);
    [[maybe_unused]] auto h3 =
        system_->scheduler()->schedule_after(cb, 3'000'000LL);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (count.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(count.load(), 3);
}

TEST_F(TimerPlaneIntegrationTest, CancelInvalidHandleIsSafe) {
    // Cancelling an invalid handle should not crash.
    system_->scheduler()->cancel_timer(sched::TimerHandle{});
    system_->scheduler()->cancel_timer(sched::TimerHandle{999999});
    SUCCEED();
}

TEST_F(TimerPlaneIntegrationTest, ScheduleZeroDelay) {
    std::atomic<bool> fired{false};
    [[maybe_unused]] auto handle = system_->scheduler()->schedule_after(
        [&fired]() { fired.store(true); }, 0LL);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fired.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(fired.load());
}

} // namespace
} // namespace hpactor
