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

// Tests for deterministic scheduler worker control API.

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

using namespace hpactor;

// ── Test Actor ────────────────────────────────────────────────────────

class CountingActor : public EventBasedActor {
  public:
    CountingActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    int received() const {
        return counter_.load();
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage&) { counter_.fetch_add(1); }};
    }

  private:
    std::atomic<int> counter_{0};
};

// ── SchedulerControlTest fixture ──────────────────────────────────────

class SchedulerControlTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.scheduler_start_paused = true;
        cfg.enable_network = false;
        cfg.enable_receptionist = false;
    }

    Config cfg;
};

TEST_F(SchedulerControlTest, StartPausedQueuesSpawnButDoesNotDispatch) {
    cfg.scheduler_threads = 1;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* sched = system.scheduler();
    ASSERT_NE(sched, nullptr);
    EXPECT_TRUE(sched->workers_paused());

    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    auto* ca = static_cast<CountingActor*>(actor.get().get());
    EXPECT_EQ(ca->received(), 0);

    bool executed = sched->run_one_ready();
    EXPECT_TRUE(executed);
    EXPECT_EQ(ca->received(), 1);

    bool no_more = sched->run_one_ready();
    EXPECT_FALSE(no_more);
}

TEST_F(SchedulerControlTest, DrainReadyProcessesQueuedMessages) {
    cfg.scheduler_threads = 1;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* sched = system.scheduler();
    auto* ca = static_cast<CountingActor*>(actor.get().get());

    const int num_msgs = 10;
    for (int i = 0; i < num_msgs; ++i) {
        system.deliver_local(actor.id(),
                             TypedMessage(TypeTag::User, StreamBuffer{1}));
    }
    EXPECT_EQ(ca->received(), 0);

    auto result = sched->drain_ready(num_msgs * 2);
    EXPECT_EQ(result.executed, static_cast<size_t>(num_msgs));
    EXPECT_TRUE(result.idle);
    EXPECT_EQ(ca->received(), num_msgs);
}

TEST_F(SchedulerControlTest, ResumeWorkersLetsAutomaticWorkersRun) {
    cfg.scheduler_threads = 1;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* sched = system.scheduler();
    auto* ca = static_cast<CountingActor*>(actor.get().get());

    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_EQ(ca->received(), 0);

    sched->resume_workers();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ca->received() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(ca->received(), 1);
}

TEST_F(SchedulerControlTest, PauseWorkersWaitsForInFlightDispatch) {
    cfg.scheduler_threads = 1;
    ActorSystem system(cfg);

    auto* sched = system.scheduler();
    EXPECT_TRUE(sched->workers_paused());

    sched->resume_workers();
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    sched->pause_workers();
    EXPECT_TRUE(sched->workers_paused());

    sched->pause_workers();
    EXPECT_TRUE(sched->workers_paused());
}

TEST_F(SchedulerControlTest, RunOneReadyRejectsWhenWorkersNotPaused) {
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = false;
    ActorSystem system(cfg);
    auto* sched = system.scheduler();

    EXPECT_FALSE(sched->workers_paused());
    bool executed = sched->run_one_ready();
    EXPECT_FALSE(executed);
}

TEST_F(SchedulerControlTest, PinnedActorRequeueStaysOnPinnedWorker) {
    cfg.scheduler_threads = 1;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* sched = system.scheduler();
    auto* ca = static_cast<CountingActor*>(actor.get().get());

    ASSERT_NE(sched, nullptr);

    // Drain the spawn-time readiness item, which has no mailbox work.
    static_cast<void>(sched->run_one_ready());

    sched->pin_actor_to_worker(actor.id(), 0);

    for (int i = 0; i < 3; ++i) {
        system.deliver_local(actor.id(),
                             TypedMessage(TypeTag::User, StreamBuffer{1}));
    }

    EXPECT_EQ(ca->received(), 0);
    EXPECT_TRUE(sched->run_actor(actor.id()));
    EXPECT_EQ(ca->received(), 1);
    EXPECT_TRUE(sched->run_actor(actor.id()));
    EXPECT_EQ(ca->received(), 2);
    EXPECT_TRUE(sched->run_actor(actor.id()));
    EXPECT_EQ(ca->received(), 3);
    EXPECT_FALSE(sched->run_actor(actor.id()));
}

TEST_F(SchedulerControlTest, YieldFromRunningActorRequeuesAdmittedWork) {
    cfg.scheduler_threads = 1;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* sched = system.scheduler();
    auto* ca = static_cast<CountingActor*>(actor.get().get());
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());

    ASSERT_NE(sched, nullptr);

    // Drain the spawn-time readiness item, which has no mailbox work.
    static_cast<void>(sched->run_one_ready());
    EXPECT_TRUE(eba->actor_state().is_idle());

    eba->actor_state().set(ActorState::kRunning);
    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_EQ(ca->received(), 0);

    sched->yield(actor.id(), 0);

    EXPECT_TRUE(eba->actor_state().is_ready());
    EXPECT_TRUE(sched->run_one_ready());
    EXPECT_EQ(ca->received(), 1);
}