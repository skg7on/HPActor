// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Tests for deterministic scheduler worker control API.

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

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

// ── Tests ─────────────────────────────────────────────────────────────

static void test_start_paused_queues_spawn_but_does_not_dispatch() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* sched = system.scheduler();
    assert(sched != nullptr);
    assert(sched->workers_paused());

    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    auto* ca = static_cast<CountingActor*>(actor.get().get());
    assert(ca->received() == 0);

    bool executed = sched->run_one_ready();
    assert(executed);
    assert(ca->received() == 1);

    bool no_more = sched->run_one_ready();
    assert(!no_more);

    std::cout << "PASS: test_start_paused_queues_spawn_but_does_not_dispatch\n";
}

static void test_drain_ready_processes_queued_messages() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* sched = system.scheduler();
    auto* ca = static_cast<CountingActor*>(actor.get().get());

    const int num_msgs = 10;
    for (int i = 0; i < num_msgs; ++i) {
        system.deliver_local(actor.id(),
                             TypedMessage(TypeTag::User, StreamBuffer{1}));
    }
    assert(ca->received() == 0);

    auto result = sched->drain_ready(num_msgs * 2);
    assert(result.executed == static_cast<size_t>(num_msgs));
    assert(result.idle == true);
    assert(ca->received() == num_msgs);

    std::cout << "PASS: test_drain_ready_processes_queued_messages\n";
}

static void test_resume_workers_lets_automatic_workers_run() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* sched = system.scheduler();
    auto* ca = static_cast<CountingActor*>(actor.get().get());

    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    assert(ca->received() == 0);

    sched->resume_workers();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ca->received() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    assert(ca->received() == 1);

    std::cout << "PASS: test_resume_workers_lets_automatic_workers_run\n";
}

static void test_pause_workers_waits_for_in_flight_dispatch() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    auto* sched = system.scheduler();
    assert(sched->workers_paused());

    sched->resume_workers();
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    sched->pause_workers();
    assert(sched->workers_paused());

    sched->pause_workers();
    assert(sched->workers_paused());

    std::cout << "PASS: test_pause_workers_waits_for_in_flight_dispatch\n";
}

static void test_run_one_ready_rejects_when_workers_not_paused() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = false;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto* sched = system.scheduler();

    assert(!sched->workers_paused());
    bool executed = sched->run_one_ready();
    assert(!executed);

    std::cout << "PASS: test_run_one_ready_rejects_when_workers_not_paused\n";
}

// ── main ──────────────────────────────────────────────────────────────

int main() {
    test_start_paused_queues_spawn_but_does_not_dispatch();
    test_drain_ready_processes_queued_messages();
    test_resume_workers_lets_automatic_workers_run();
    test_pause_workers_waits_for_in_flight_dispatch();
    test_run_one_ready_rejects_when_workers_not_paused();
    std::cout << "All scheduler control tests passed.\n";
    return 0;
}
