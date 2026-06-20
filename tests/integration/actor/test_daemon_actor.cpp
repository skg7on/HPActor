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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/daemon_actor.hpp>

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace hpactor;

// Minimal DaemonActor for testing
class TestDaemon : public DaemonActor {
  public:
    TestDaemon(ActorContext* ctx, ActorSystem& sys) : DaemonActor(ctx, sys) {}

    std::atomic<int> iterations{0};
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};

    bool run_once() override {
        started.store(true);
        iterations.fetch_add(1);
        if (iterations.load() >= 10)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return true;
    }

    void on_daemon_start() override {
        started.store(true);
    }

    void on_daemon_stop() override {
        stopped.store(true);
    }
};

class DaemonActorIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config;
        config.enable_network = false;
        system_ = std::make_unique<ActorSystem>(config);
    }
    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(100);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }
    std::unique_ptr<ActorSystem> system_;
};

TEST_F(DaemonActorIntegrationTest, DispatchPolicyAndRun) {
    auto actor = system_->spawn<TestDaemon>();
    auto* raw = static_cast<TestDaemon*>(actor.get().get());
    ASSERT_NE(raw, nullptr);

    EXPECT_EQ(raw->dispatch_policy(), sched::DispatchPolicy::DedicatedThread);

    // Wait for daemon to start and run
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !raw->stopped.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(raw->started.load());
    EXPECT_GT(raw->iterations.load(), 0);
    EXPECT_TRUE(raw->stopped.load());
}

TEST_F(DaemonActorIntegrationTest, CpuAffinity) {
    auto actor = system_->spawn<TestDaemon>();
    auto* raw = static_cast<TestDaemon*>(actor.get().get());
    raw->set_cpu_affinity(2);
    auto hints = raw->dispatch_hints();
    EXPECT_EQ(hints.cpu_affinity, 2);
}
