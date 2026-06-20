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

// Tests for actor state transfer with non-coroutine (behavior-based) actors.
// Verifies that ActorState CAS gating prevents double-execution in the
// M:N cooperative scheduling path.

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_state.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <scheduler_test_driver.hpp>

using namespace hpactor;

// Test actor that counts received messages
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

// ─── ActorStateTransferTest fixture ───────────────────────────────────

class ActorStateTransferTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.scheduler_start_paused = true;
        cfg.enable_network = false;
    }

    Config cfg;
};

TEST_F(ActorStateTransferTest, EventBasedActorHasActorState) {
    cfg.scheduler_threads = 0; // no scheduler — keep actor in post-spawn state
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    auto& state = eba->actor_state();

    EXPECT_TRUE(state.is_idle() || state.is_ready());
}

TEST_F(ActorStateTransferTest, ActorStateCasTransitions) {
    ActorState state;
    EXPECT_TRUE(state.is_idle());

    // Idle -> Ready
    uint32_t expected = ActorState::kIdle;
    bool ok = state.cas(expected, ActorState::kReady);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(state.is_ready());

    // Ready -> Running
    expected = ActorState::kReady;
    ok = state.cas(expected, ActorState::kRunning);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(state.is_running());

    // Running -> Idle
    expected = ActorState::kRunning;
    ok = state.cas(expected, ActorState::kIdle);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(state.is_idle());

    // Failed CAS (wrong expected) — state unchanged
    state.set(ActorState::kRunning);
    expected = ActorState::kIdle;
    ok = state.cas(expected, ActorState::kReady);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(state.is_running());

    // Terminated
    state.set(ActorState::kTerminated);
    EXPECT_TRUE(state.is_terminated());
}

TEST_F(ActorStateTransferTest, BehaviorActorReceivesMessages) {
    cfg.scheduler_threads = 2;
    ActorSystem system(cfg);
    hpactor::test::SchedulerTestDriver driver(system);

    auto actor = system.spawn<CountingActor>();
    auto addr = actor.address();

    auto sender = system.spawn<EventBasedActor>();
    ActorContext ctx(sender, &system);

    constexpr int kMsgCount = 100;
    for (int i = 0; i < kMsgCount; ++i) {
        TypedMessage msg(TypeTag::User, StreamBuffer{1});
        msg.set_sender_address(sender.address());
        ctx.send(addr, std::move(msg));
    }

    auto* ca = static_cast<CountingActor*>(actor.get().get());
    bool done = driver.drain_until([&] { return ca->received() == kMsgCount; });
    EXPECT_TRUE(done);
}

TEST_F(ActorStateTransferTest, ConcurrentSendsSingleActor) {
    cfg.scheduler_threads = 4;
    ActorSystem system(cfg);
    hpactor::test::SchedulerTestDriver driver(system);

    auto actor = system.spawn<CountingActor>();
    auto addr = actor.address();

    constexpr int kThreads = 4;
    constexpr int kMsgsPerThread = 50;
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&system, &addr]() {
            auto local_sender = system.spawn<EventBasedActor>();
            ActorContext ctx(local_sender, &system);

            for (int i = 0; i < kMsgsPerThread; ++i) {
                TypedMessage msg(TypeTag::User, StreamBuffer{1});
                msg.set_sender_address(local_sender.address());
                ctx.send(addr, std::move(msg));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto* ca = static_cast<CountingActor*>(actor.get().get());
    int expected = kThreads * kMsgsPerThread;
    bool done = driver.drain_until([&] { return ca->received() == expected; });
    EXPECT_TRUE(done);
}

TEST_F(ActorStateTransferTest, StatePreventsDoubleExecution) {
    ActorState state;

    // Worker 1: CAS Idle -> Ready -> Running
    uint32_t expected = ActorState::kIdle;
    bool ok = state.cas(expected, ActorState::kReady);
    EXPECT_TRUE(ok);

    expected = ActorState::kReady;
    ok = state.cas(expected, ActorState::kRunning);
    EXPECT_TRUE(ok);

    // Worker 2: CAS Ready -> Running FAILS (state is Running)
    expected = ActorState::kReady;
    ok = state.cas(expected, ActorState::kRunning);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(state.is_running());

    // Worker 1 finishes, sets Idle
    state.set(ActorState::kIdle);

    // Next worker: CAS Idle -> Ready succeeds
    expected = ActorState::kIdle;
    ok = state.cas(expected, ActorState::kReady);
    EXPECT_TRUE(ok);
}