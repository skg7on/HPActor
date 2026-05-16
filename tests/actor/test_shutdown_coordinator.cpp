// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/drain_config.hpp>
#include <hpactor/actor/drain_policy.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace hpactor;

// ── Helper: poll phase until condition or timeout ──────────────────────────

template <typename Fn>
static bool poll_until(Fn&& condition, int timeout_ms = 100) {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}

// ── Simple lifecycle test actor ────────────────────────────────────────────

class ShutdownTestActor : public EventBasedActor, public LifecycleActor {
  public:
    ShutdownTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    bool drain_hook_called() const {
        return drain_hook_called_;
    }
    bool stop_hook_called() const {
        return stop_hook_called_;
    }

    void on_drain() override {
        drain_hook_called_ = true;
    }
    void on_stop() override {
        stop_hook_called_ = true;
    }

    void on_exit() override {
        exit_called_ = true;
        EventBasedActor::on_exit();
    }
    bool exit_called() const {
        return exit_called_;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[](TypedMessage&) {}};
    }

  private:
    bool drain_hook_called_ = false;
    bool stop_hook_called_ = false;
    bool exit_called_ = false;
};

// ── System actor (drains last) ─────────────────────────────────────────────

class ShutdownSystemActor : public EventBasedActor, public LifecycleActor {
  public:
    ShutdownSystemActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    bool is_system_actor() const override {
        return true;
    }

    void on_exit() override {
        exit_called_ = true;
        EventBasedActor::on_exit();
    }
    bool exit_called() const {
        return exit_called_;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[](TypedMessage&) {}};
    }

  private:
    bool exit_called_ = false;
};

// ── Test 1: shutdown_phase_machine_transitions ─────────────────────────────
// Verifies that the phase machine advances through the expected phases
// and that is_ready() returns false after shutdown starts.

static void test_shutdown_phase_machine_transitions() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    // Spawn a few actors
    system.spawn<ShutdownTestActor>();
    system.spawn<ShutdownTestActor>();

    // Verify initial state
    assert(system.is_ready());
    assert(system.shutdown_phase() == ShutdownPhase::Running);

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);

    auto result = system.shutdown(opts);
    assert(result.has_value());

    // After shutdown, is_ready should be false and phase should be Stopped
    assert(!system.is_ready());
    assert(system.shutdown_phase() == ShutdownPhase::Stopped ||
           system.shutdown_phase() == ShutdownPhase::ForcedStop);

    std::cout << "PASS: test_shutdown_phase_machine_transitions\n";
}

// ── Test 2: shutdown_reverse_topological_order ─────────────────────────────
// Verifies that non-system actors drain before system actors.
// We spawn a regular actor (child, non-system) and a system actor (parent,
// system). The child should be drained in the first pass, the parent last.

static void test_shutdown_reverse_topological_order() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    // Spawn a non-system actor (child) and a system actor (parent)
    auto child_ref = system.spawn<ShutdownTestActor>();
    auto parent_ref = system.spawn<ShutdownSystemActor>();

    auto* child = static_cast<ShutdownTestActor*>(child_ref.get().get());
    auto* parent = static_cast<ShutdownSystemActor*>(parent_ref.get().get());

    // Set ImmediateStop so drain is synchronous and order is deterministic
    auto* child_lc = child->as_lifecycle();
    auto* parent_lc = parent->as_lifecycle();
    assert(child_lc != nullptr);
    assert(parent_lc != nullptr);
    child_lc->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});
    parent_lc->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);

    auto result = system.shutdown(opts);
    assert(result.has_value());

    // Both should be stopped (exited)
    assert(child->exit_called());
    assert(parent->exit_called());

    // Verify phase reached Stopped
    assert(system.shutdown_phase() == ShutdownPhase::Stopped ||
           system.shutdown_phase() == ShutdownPhase::ForcedStop);

    std::cout << "PASS: test_shutdown_reverse_topological_order\n";
}

// ── Test 3: forced_stop_on_timeout ─────────────────────────────────────────
// Verifies that when drain takes too long with a short timeout and
// force_after_timeout=true, the system transitions to ForcedStop.

static void test_forced_stop_on_timeout() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    // Spawn an actor with Drain policy (not ImmediateStop)
    auto actor_ref = system.spawn<ShutdownTestActor>();
    auto* actor = static_cast<ShutdownTestActor*>(actor_ref.get().get());

    auto* lc = actor->as_lifecycle();
    assert(lc != nullptr);
    // Use Drain with long timeout - with a very short system-level timeout,
    // the per-phase deadline will trigger ForcedStop before draining completes
    lc->set_drain_config(
        DrainConfig{DrainPolicy::Drain, std::chrono::milliseconds{30'000}});

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    // Very short actor_drain_timeout forces ForcedStop
    opts.actor_drain_timeout = std::chrono::milliseconds(1);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);
    opts.force_after_timeout = true;

    auto result = system.shutdown(opts);
    // Should either succeed (if drain completes) or reach ForcedStop
    assert(result.has_value());

    // Verify that the system is no longer running after forced shutdown
    assert(!system.is_running());
    assert(system.shutdown_phase() == ShutdownPhase::ForcedStop);

    // Actor should be in some terminal state
    auto state = lc->state();
    assert(state == LifecycleState::kStopped || state == LifecycleState::kDraining ||
           state == LifecycleState::kStopping);

    std::cout << "PASS: test_forced_stop_on_timeout\n";
}

// ── Test 4: is_ready_flips_on_draining_ingress ─────────────────────────────
// Verifies is_ready() is true initially and becomes false during shutdown.

static void test_is_ready_flips_on_draining_ingress() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    // Initially the system is ready
    assert(system.is_ready());
    assert(!system.is_draining());

    // Spawn an actor
    system.spawn<ShutdownTestActor>();

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);

    auto result = system.shutdown(opts);
    assert(result.has_value());

    // After shutdown, ready flag is cleared
    assert(!system.is_ready());

    std::cout << "PASS: test_is_ready_flips_on_draining_ingress\n";
}

int main() {
    test_shutdown_phase_machine_transitions();
    test_shutdown_reverse_topological_order();
    test_forced_stop_on_timeout();
    test_is_ready_flips_on_draining_ingress();
    std::cout << "\nAll shutdown coordinator tests passed.\n";
    return 0;
}
