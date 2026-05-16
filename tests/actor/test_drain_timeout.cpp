// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace hpactor;

// ── Mock scheduler that captures timer callbacks ─────────────────────────

class MockScheduler : public sched::IScheduler {
  public:
    sched::TimerHandle schedule_after(sched::timer_callback cb, int64_t) override {
        uint64_t id = next_id_++;
        callbacks_[id] = std::move(cb);
        return sched::TimerHandle{id};
    }
    sched::TimerHandle
    schedule_every(sched::timer_callback cb, int64_t interval_ns) override {
        return schedule_after(std::move(cb), interval_ns);
    }
    void cancel_timer(sched::TimerHandle handle) override {
        cancelled_.insert(handle.id);
        callbacks_.erase(handle.id);
    }
    void notify_ready(ActorId, uint8_t, int64_t) override {}
    void notify_idle(ActorId) override {}
    void yield(ActorId, uint8_t) override {}
    void start() override {}
    void stop() override {}
    size_t worker_count() const override {
        return 1;
    }
    bool is_running() const override {
        return true;
    }
    void register_dedicated_thread(ActorId, int) override {}
    void register_dedicated_pool(ActorId, uint32_t) override {}
    void unregister_dedicated(ActorId) override {}

    // Invoke a previously scheduled timer callback by its timer ID.
    void invoke_timer(uint64_t id) {
        auto it = callbacks_.find(id);
        if (it != callbacks_.end()) {
            auto cb = std::move(it->second);
            callbacks_.erase(it);
            cb();
        }
    }

    // Returns true if a timer with the given ID was cancelled.
    bool is_cancelled(uint64_t id) const {
        return cancelled_.count(id) > 0;
    }

    // Returns the number of pending (not yet fired, not cancelled) timers.
    size_t pending_timers() const {
        return callbacks_.size();
    }

    std::unordered_map<uint64_t, sched::timer_callback> callbacks_;
    std::unordered_set<uint64_t> cancelled_;
    uint64_t next_id_ = 1;
};

// ── Test actor ───────────────────────────────────────────────────────────

class DrainTimeoutTestActor : public EventBasedActor, public LifecycleActor {
  public:
    int handler_count = 0;

    DrainTimeoutTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (static_cast<uint32_t>(msg.type_id()) >= 0x1000) {
                handler_count++;
            }
        }};
    }
};

// ── Helpers ──────────────────────────────────────────────────────────────

static void inject_user_message(EventBasedActor* actor) {
    auto* mailbox = actor->get_mailbox();
    assert(mailbox != nullptr);

    static_assert(sizeof(TypedMessage) <= 1024, "TypedMessage must fit in 1024 "
                                                "bytes for stack allocation");

    auto* node = static_cast<TypedMessage*>(mem::allocate(
        mem::RegionType::kMessage, sizeof(TypedMessage), actor->id()));
    new (node) TypedMessage(TypeTag(0x1001), StreamBuffer{});
    node->set_sender_address(ActorAddress{});

    mailbox->inject_for_test(node);
}

static uint32_t dlq_depth(ActorSystem& system) {
    auto snapshot = system.dead_letter_snapshot();
    return snapshot.depth;
}

// ── Test 1: drain timeout forces transition and dead-letters messages ────

static void test_drain_timeout_forces_transition() {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto ref = system.spawn<DrainTimeoutTestActor>();
    auto* actor = static_cast<DrainTimeoutTestActor*>(ref.get().get());

    auto* lc = actor->as_lifecycle();
    assert(lc != nullptr);

    // Install mock scheduler so we control timer invocation.
    MockScheduler mock_sched;
    actor->set_scheduler(&mock_sched);

    // Configure drain with a very short timeout.
    lc->set_drain_config(
        DrainConfig{DrainPolicy::Drain, std::chrono::milliseconds{1}});

    // Enqueue 10 user messages — more than can be processed before timeout.
    for (int i = 0; i < 10; ++i) {
        inject_user_message(actor);
    }

    // Transition to kDraining.
    bool ok = lc->transition(LifecycleState::kDraining);
    assert(ok);

    // Start the drain timer — this schedules a callback on the mock scheduler.
    actor->start_drain_timer();
    assert(mock_sched.pending_timers() == 1);

    // Simulate the timer firing.
    uint64_t timer_id = mock_sched.next_id_ - 1;
    mock_sched.invoke_timer(timer_id);

    // After timeout, actor should be Stopped (not Draining).
    assert(lc->state() == LifecycleState::kStopped);

    // All messages should have been dead-lettered by drain_all_immediate().
    assert(actor->handler_count == 0);
    assert(dlq_depth(system) == 10);

    // Mailbox should be empty.
    assert(actor->mailbox_is_empty());

    std::cout << "PASS: test_drain_timeout_forces_transition\n";
}

// ── Test 2: drain completes before timeout, timer is cancelled ───────────

static void test_drain_completes_before_timeout_cancels_timer() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto ref = system.spawn<DrainTimeoutTestActor>();
    auto* actor = static_cast<DrainTimeoutTestActor*>(ref.get().get());

    auto* lc = actor->as_lifecycle();
    assert(lc != nullptr);

    // Install mock scheduler so we can verify cancellation.
    MockScheduler mock_sched;
    actor->set_scheduler(&mock_sched);

    // Configure drain with a very long timeout (won't fire during test).
    lc->set_drain_config(
        DrainConfig{DrainPolicy::Drain, std::chrono::milliseconds{5000}});

    // Enqueue 1 user message — drain should finish quickly.
    inject_user_message(actor);

    // Transition to kDraining.
    bool ok = lc->transition(LifecycleState::kDraining);
    assert(ok);

    // Start the drain timer.
    actor->start_drain_timer();
    uint64_t timer_id = mock_sched.next_id_ - 1;
    assert(mock_sched.pending_timers() == 1);

    // Process the single message — this should complete drain naturally.
    TypedMessage msg;
    auto* mailbox = actor->get_mailbox();
    bool found = mailbox->try_pop(msg);
    assert(found);
    actor->receive(msg);

    // After processing, drain should complete:
    // handler count = 1 (message was processed)
    assert(actor->handler_count == 1);

    // State should be Stopped (drain completed naturally).
    assert(lc->state() == LifecycleState::kStopped);

    // The drain timer should have been cancelled.
    assert(mock_sched.is_cancelled(timer_id));
    assert(mock_sched.pending_timers() == 0);

    // Mailbox should be empty.
    assert(actor->mailbox_is_empty());

    std::cout << "PASS: test_drain_completes_before_timeout_cancels_timer\n";
}

int main() {
    test_drain_timeout_forces_transition();
    test_drain_completes_before_timeout_cancels_timer();
    std::cout << "\nAll drain timeout tests passed.\n";
    return 0;
}
