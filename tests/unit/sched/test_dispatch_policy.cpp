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

#include <atomic>
#include <chrono>
#include <climits>
#include <gtest/gtest.h>
#include <thread>

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/actor/dense_computing_actor.hpp>
#include <hpactor/actor/polling_actor.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/sched/scheduler.hpp>

using namespace hpactor;

class PoolProbeActor : public DenseComputingActor {
  public:
    PoolProbeActor(ActorContext* ctx, ActorSystem& sys)
        : DenseComputingActor(ctx, sys, /*pool_size=*/2) {}

    std::atomic<bool> handled{false};
    std::atomic<uint32_t> worker_id{0};

    void on_activate() override {
        become(Behavior{[this](TypedMessage&) {
            auto* scheduler =
                static_cast<sched::HybridScheduler*>(system().scheduler());
            worker_id.store(scheduler->current_worker_id(),
                            std::memory_order_release);
            handled.store(true, std::memory_order_release);
        }});
        EventBasedActor::on_activate();
    }
};

static void wait_for_probe(PoolProbeActor* actor) {
    for (int i = 0; i < 50 && !actor->handled.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ── DispatchPolicyTest fixture ───────────────────────────────────────

class DispatchPolicyTest : public ::testing::Test {
  protected:
    void SetUp() override {
        config.enable_network = false;
        config.scheduler_threads = 2;
    }

    Config config;
};

TEST_F(DispatchPolicyTest, DefaultEventBasedActorIsCooperative) {
    ActorSystem system(config);

    auto actor = system.spawn<EventBasedActor>();
    auto* raw = static_cast<EventBasedActor*>(actor.get().get());
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->dispatch_policy(), sched::DispatchPolicy::Cooperative);
}

TEST_F(DispatchPolicyTest, DaemonActorIsDedicatedThread) {
    ActorSystem system(config);

    class SimpleDaemon : public DaemonActor {
      public:
        SimpleDaemon(ActorContext* ctx, ActorSystem& sys)
            : DaemonActor(ctx, sys) {}
        std::atomic<bool> ran{false};
        bool run_once() override {
            ran.store(true);
            return false; // one-shot
        }
    };

    auto actor = system.spawn<SimpleDaemon>();
    auto* raw = static_cast<SimpleDaemon*>(actor.get().get());
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->dispatch_policy(), sched::DispatchPolicy::DedicatedThread);

    // Wait for daemon to run
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !raw->ran.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_TRUE(raw->ran.load());
}

TEST_F(DispatchPolicyTest, PollingActorIsDedicatedThread) {
    ActorSystem system(config);

    class SimplePoller : public PollingActor {
      public:
        SimplePoller(ActorContext* ctx, ActorSystem& sys)
            : PollingActor(ctx, sys, /*cpu_core=*/-1) {}
        bool run_once() override {
            return false; // one-shot
        }
    };

    auto actor = system.spawn<SimplePoller>();
    auto* raw = static_cast<SimplePoller*>(actor.get().get());
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->dispatch_policy(), sched::DispatchPolicy::DedicatedThread);
}

TEST_F(DispatchPolicyTest, DenseComputingActorIsDedicatedPool) {
    ActorSystem system(config);

    auto actor = system.spawn<DenseComputingActor>(/*pool_size=*/4);
    auto* raw = static_cast<DenseComputingActor*>(actor.get().get());
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->dispatch_policy(), sched::DispatchPolicy::DedicatedPool);

    auto hints = raw->dispatch_hints();
    EXPECT_EQ(hints.pool_size, 4U);
}

TEST_F(DispatchPolicyTest,
       DedicatedPoolActorsAreDispatchedOutsideCooperativeWorkers) {
    ActorSystem system(config);

    auto actor = system.spawn<PoolProbeActor>();
    auto* raw = static_cast<PoolProbeActor*>(actor.get().get());
    ASSERT_NE(raw, nullptr);

    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{}));
    wait_for_probe(raw);

    EXPECT_TRUE(raw->handled.load(std::memory_order_acquire));
    EXPECT_EQ(raw->worker_id.load(std::memory_order_acquire), UINT32_MAX);
}

TEST_F(DispatchPolicyTest, SpawnConfiguredPreservesActorDispatchPolicy) {
    ActorSystem system(config);

    config::ActorDef def;
    def.behavior = "PoolProbeActor";

    auto actor = system.spawn_configured(
        std::make_shared<PoolProbeActor>(nullptr, system), def);
    auto* raw = static_cast<PoolProbeActor*>(actor.get().get());
    ASSERT_NE(raw, nullptr);

    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{}));
    wait_for_probe(raw);

    EXPECT_TRUE(raw->handled.load(std::memory_order_acquire));
    EXPECT_EQ(raw->worker_id.load(std::memory_order_acquire), UINT32_MAX);
}

TEST(DispatchPolicyDefaultsTest, DispatchHintsDefaultValues) {
    sched::DispatchHints hints;
    EXPECT_EQ(hints.cpu_affinity, -1);
    EXPECT_EQ(hints.pool_size, 1U);
    EXPECT_EQ(hints.priority, 0);
}
