// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Tests for actor state transfer with non-coroutine (behavior-based) actors.
// Verifies that ActorState CAS gating prevents double-execution in the
// M:N cooperative scheduling path.

#include <hpactor/actor/actor_state.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <scheduler_test_driver.hpp>

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

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

// ─── Tests ────────────────────────────────────────────────────────────

static void test_event_based_actor_has_actor_state() {
    Config cfg;
    cfg.scheduler_threads = 0; // no scheduler — keep actor in post-spawn state
    cfg.enable_network = false;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    auto& state = eba->actor_state();

    assert(state.is_idle() || state.is_ready());
    std::cout << "PASS: test_event_based_actor_has_actor_state\n";
}

static void test_actor_state_cas_transitions() {
    ActorState state;
    assert(state.is_idle());

    // Idle → Ready
    uint32_t expected = ActorState::kIdle;
    bool ok = state.cas(expected, ActorState::kReady);
    assert(ok);
    assert(state.is_ready());

    // Ready → Running
    expected = ActorState::kReady;
    ok = state.cas(expected, ActorState::kRunning);
    assert(ok);
    assert(state.is_running());

    // Running → Idle
    expected = ActorState::kRunning;
    ok = state.cas(expected, ActorState::kIdle);
    assert(ok);
    assert(state.is_idle());

    // Failed CAS (wrong expected) — state unchanged
    state.set(ActorState::kRunning);
    expected = ActorState::kIdle;
    ok = state.cas(expected, ActorState::kReady);
    assert(!ok);
    assert(state.is_running());

    // Terminated
    state.set(ActorState::kTerminated);
    assert(state.is_terminated());

    std::cout << "PASS: test_actor_state_cas_transitions\n";
}

static void test_behavior_actor_receives_messages() {
    Config cfg;
    cfg.scheduler_threads = 2;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
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
    assert(done);
    std::cout << "PASS: test_behavior_actor_receives_messages ("
              << ca->received() << "/" << kMsgCount << ")\n";
}

static void test_concurrent_sends_single_actor() {
    Config cfg;
    cfg.scheduler_threads = 4;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
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
    assert(done);
    std::cout << "PASS: test_concurrent_sends_single_actor (" << ca->received()
              << "/" << expected << ")\n";
}

static void test_state_prevents_double_execution() {
    ActorState state;

    // Worker 1: CAS Idle → Ready → Running
    uint32_t expected = ActorState::kIdle;
    bool ok = state.cas(expected, ActorState::kReady);
    assert(ok);

    expected = ActorState::kReady;
    ok = state.cas(expected, ActorState::kRunning);
    assert(ok);

    // Worker 2: CAS Ready → Running FAILS (state is Running)
    expected = ActorState::kReady;
    ok = state.cas(expected, ActorState::kRunning);
    assert(!ok);
    assert(state.is_running());

    // Worker 1 finishes, sets Idle
    state.set(ActorState::kIdle);

    // Next worker: CAS Idle → Ready succeeds
    expected = ActorState::kIdle;
    ok = state.cas(expected, ActorState::kReady);
    assert(ok);

    std::cout << "PASS: test_state_prevents_double_execution\n";
}

int main() {
    test_actor_state_cas_transitions();
    test_state_prevents_double_execution();
    test_event_based_actor_has_actor_state();
    test_behavior_actor_receives_messages();
    test_concurrent_sends_single_actor();
    std::cout << "\nAll actor state transfer tests passed.\n";
    return 0;
}
