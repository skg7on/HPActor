// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/messages.pb.h>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace hpactor;

// ── Helper: poll until condition is true or timeout expires
// ────────────────────

template <typename Fn>
static bool poll_until(Fn&& condition, int timeout_ms = 2000) {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

// ── StopTestActor: lifecycle actor with ImmediateStop policy
// ───────────────────

class StopTestActor : public EventBasedActor, public LifecycleActor {
  public:
    StopTestActor(ActorContext* ctx, ActorSystem& sys, DrainConfig drain_cfg = {})
        : EventBasedActor(ctx, sys) {
        set_drain_config(drain_cfg);
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    bool exit_called() const {
        return exit_called_;
    }
    int down_count() const {
        return down_count_;
    }

    void on_exit() override {
        exit_called_ = true;
        EventBasedActor::on_exit();
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == TypeTag::DownMsg) {
                ++down_count_;
            }
        }};
    }

  private:
    bool exit_called_ = false;
    int down_count_ = 0;
};

// ── NonLifecycleActor: no lifecycle, just counts exit calls
// ────────────────────

class NonLifecycleActor : public EventBasedActor {
  public:
    NonLifecycleActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    bool exit_called() const {
        return exit_called_;
    }

    void on_exit() override {
        exit_called_ = true;
        EventBasedActor::on_exit();
    }

  private:
    bool exit_called_ = false;
};

// ── Test 1: stop() with ImmediateStop transitions to kStopped and sends
// DownMsg ─

static void test_stop_async_transitions_to_stopped() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    auto target_ref =
        system.spawn<StopTestActor>(DrainConfig{DrainPolicy::ImmediateStop});
    auto* target = static_cast<StopTestActor*>(target_ref.get().get());
    auto* lc = target->as_lifecycle();
    assert(lc != nullptr);
    assert(lc->state() == LifecycleState::kActive);

    // Spawn a monitor actor linked to the target
    auto monitor_ref =
        system.spawn<StopTestActor>(DrainConfig{DrainPolicy::ImmediateStop});
    auto* monitor = static_cast<StopTestActor*>(monitor_ref.get().get());
    // Directly register the monitor in the target's monitored_ list.
    // Avoids async MonitorMsg protocol which requires scheduler dispatch.
    target->context()->add_monitored(monitor->address());

    // Create an ActorContext with system pointer to call stop()
    ActorContext ctx(Actor{}, &system);

    // Call stop()
    ctx.stop(target_ref.id());

    // Target should be kStopped immediately (ImmediateStop is synchronous)
    assert(lc->state() == LifecycleState::kStopped);
    assert(target->exit_called());

    // Monitor mailbox should contain a DownMsg
    auto* mbox = system.get_mailbox(monitor_ref.id());
    assert(mbox != nullptr);
    bool has_down = false;
    TypedMessage msg;
    while (mbox->try_pop(msg)) {
        if (msg.type_id() == TypeTag::DownMsg) {
            has_down = true;
        }
    }
    assert(has_down);

    std::cout << "PASS: test_stop_async_transitions_to_stopped\n";
}

// ── Test 2: stop_sync() with ImmediateStop returns ok
// ──────────────────────────

static void test_stop_sync_blocks_until_stopped() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    auto target_ref =
        system.spawn<StopTestActor>(DrainConfig{DrainPolicy::ImmediateStop});
    auto* target = static_cast<StopTestActor*>(target_ref.get().get());
    auto* lc = target->as_lifecycle();

    ActorContext ctx(Actor{}, &system);

    auto result = ctx.stop_sync(target_ref.id(), std::chrono::milliseconds(5000));
    assert(result.has_value());
    assert(lc->state() == LifecycleState::kStopped);

    std::cout << "PASS: test_stop_sync_blocks_until_stopped\n";
}

// ── Test 3: stop_sync() with short timeout returns error
// ───────────────────────

static void test_stop_sync_timeout_returns_error() {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    // Use default DrainPolicy::Drain with 30s drain timeout.
    // stop() transitions to kDraining but drain completion requires
    // either the scheduler to empty the mailbox or the drain timer to fire.
    // With no scheduler processing and a 30s drain timeout, a 10ms
    // stop_sync should time out.
    auto target_ref = system.spawn<StopTestActor>();
    auto* lc = static_cast<StopTestActor*>(target_ref.get().get())->as_lifecycle();
    assert(lc != nullptr);

    ActorContext ctx(Actor{}, &system);

    auto result = ctx.stop_sync(target_ref.id(), std::chrono::milliseconds(10));
    assert(!result.has_value());
    assert(result.error().code() == errors::timeout);
    // Actor should still be in kDraining (not yet stopped)
    assert(lc->state() == LifecycleState::kDraining);

    std::cout << "PASS: test_stop_sync_timeout_returns_error\n";
}

// ── Test 4: stop() on non-lifecycle actor calls on_exit directly
// ───────────────

static void test_stop_no_lifecycle_calls_on_exit_directly() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    auto target_ref = system.spawn<NonLifecycleActor>();
    auto* target = static_cast<NonLifecycleActor*>(target_ref.get().get());
    assert(target->as_lifecycle() == nullptr);
    assert(!target->exit_called());

    ActorContext ctx(Actor{}, &system);

    ctx.stop(target_ref.id());

    assert(target->exit_called());

    std::cout << "PASS: test_stop_no_lifecycle_calls_on_exit_directly\n";
}

int main() {
    test_stop_async_transitions_to_stopped();
    test_stop_sync_blocks_until_stopped();
    test_stop_sync_timeout_returns_error();
    test_stop_no_lifecycle_calls_on_exit_directly();
    std::cout << "\nAll actor stop tests passed.\n";
    return 0;
}
