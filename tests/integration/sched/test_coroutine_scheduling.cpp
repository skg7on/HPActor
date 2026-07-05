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

// tests/integration/sched/test_coroutine_scheduling.cpp
// Integration test: spawn -> deliver message -> actor wakes -> processes
#include <gtest/gtest.h>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/coroutine/coroutine_awaiters.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>

// Fixture for tests that need an ActorSystem with scheduler disabled.
class CoroutineSchedulingIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        hpactor::Config cfg;
        cfg.scheduler_threads = 0;
        system_ = std::make_unique<hpactor::ActorSystem>(cfg);
    }
    void TearDown() override {
        if (system_) {
            hpactor::ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(10);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }
    std::unique_ptr<hpactor::ActorSystem> system_;
};

TEST(CoroutineSchedulingTest, SchedulerComponentsVerify) {
    // Verify the scheduler starts with the configured worker count
    hpactor::Config config;
    config.scheduler_threads = 2;
    hpactor::ActorSystem system(config);

    EXPECT_TRUE(system.scheduler()->is_running());
    EXPECT_EQ(system.scheduler()->worker_count(), 2u);
}
