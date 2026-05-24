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

// =============================================================================
// Test: Priority Scheduler Interface
// =============================================================================

#include <hpactor/core/actor_system.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <chrono>
#include <gtest/gtest.h>

using namespace hpactor;

// Fixture for tests that need an ActorSystem with scheduler disabled.
class PrioritySchedulerIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.scheduler_threads = 0;
        system_ = std::make_unique<ActorSystem>(cfg);
    }
    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(10);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }
    std::unique_ptr<ActorSystem> system_;
};

TEST(PrioritySchedulerTest, SchedulerCreation) {
    Config config{.scheduler_threads = 4,
                  .max_queue_depth = 1024,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .tracing = {}};
    ActorSystem system(config);
    ASSERT_NE(system.scheduler(), nullptr);
    EXPECT_EQ(system.scheduler()->worker_count(), 4u);
    EXPECT_TRUE(system.scheduler()->is_running());
}

TEST(PrioritySchedulerTest, NotifyReadyWithPriorities) {
    Config config{.scheduler_threads = 0,
                  .max_queue_depth = 1024,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .tracing = {}};
    ActorSystem system(config);

    ActorId actors[] = {ActorId{1}, ActorId{2}, ActorId{3}, ActorId{4}};

    // notify_ready accepts different priority levels
    system.scheduler()->notify_ready(actors[0], 3, INT64_MAX);
    system.scheduler()->notify_ready(actors[1], 0, INT64_MAX);
    system.scheduler()->notify_ready(actors[2], 1, INT64_MAX);
    system.scheduler()->notify_ready(actors[3], 2, INT64_MAX);

    SUCCEED();
}

TEST(PrioritySchedulerTest, NotifyReadyWithDeadlines) {
    Config config{.scheduler_threads = 0,
                  .max_queue_depth = 1024,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .tracing = {}};
    ActorSystem system(config);

    ActorId actors[] = {ActorId{101}, ActorId{102}, ActorId{103}};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();

    // EDF: notify_ready accepts deadlines
    system.scheduler()->notify_ready(actors[0], 2, now + 10'000'000);
    system.scheduler()->notify_ready(actors[1], 2, now + 1'000'000);
    system.scheduler()->notify_ready(actors[2], 2, now + 5'000'000);

    SUCCEED();
}

TEST(PrioritySchedulerTest, IsRunning) {
    Config config{.scheduler_threads = 1,
                  .max_queue_depth = 1024,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .tracing = {}};
    ActorSystem system(config);
    EXPECT_TRUE(system.is_running());
}
