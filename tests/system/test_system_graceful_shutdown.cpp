// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// System test: Graceful Shutdown
// Validates DrainPolicy → lifecycle → DLQ → shutdown coordinator

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>

#include "system_test_fixture.hpp"

#include <cassert>
#include <cstdio>

using namespace hpactor;

using CountingActor = test::CountingActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Shutdown with no user actors completes successfully
// ═══════════════════════════════════════════════════════════════════════════════

static void test_shutdown_no_user_actors() {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    assert(system.is_running());

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{100};
    opts.actor_drain_timeout = std::chrono::milliseconds{100};
    opts.cluster_leave_timeout = std::chrono::milliseconds{100};
    opts.force_after_timeout = true;

    auto result = system.shutdown(opts);
    assert(result.has_value());
    assert(system.shutdown_phase() == ShutdownPhase::Stopped);

    std::printf("PASS: test_shutdown_no_user_actors\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Shutdown with ImmediateStop actors drains them
// ═══════════════════════════════════════════════════════════════════════════════

static void test_shutdown_with_immediate_stop() {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* actor = static_cast<test::CountingActor*>(a1.get().get());

    // Transition to kActive so drain can proceed
    if (actor->as_lifecycle()->state() == LifecycleState::kStarting) {
        actor->as_lifecycle()->transition(LifecycleState::kActive);
    }
    actor->as_lifecycle()->set_drain_config(
        DrainConfig{DrainPolicy::ImmediateStop, std::chrono::milliseconds{500}});

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{500};
    opts.actor_drain_timeout = std::chrono::milliseconds{5000};
    opts.cluster_leave_timeout = std::chrono::milliseconds{500};
    opts.force_after_timeout = true;

    auto result = system.shutdown(opts);
    assert(result.has_value());

    // Actor should be stopped
    LifecycleState s = actor->as_lifecycle()->state();
    assert(s == LifecycleState::kStopped || s == LifecycleState::kStopping);

    std::printf("PASS: test_shutdown_with_immediate_stop\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Shutdown phase transitions are observable
// ═══════════════════════════════════════════════════════════════════════════════

static void test_shutdown_phase_transitions() {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    assert(system.shutdown_phase() == ShutdownPhase::Running);
    assert(system.is_ready());

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds{100};
    opts.actor_drain_timeout = std::chrono::milliseconds{100};
    opts.cluster_leave_timeout = std::chrono::milliseconds{100};
    opts.force_after_timeout = true;

    // shutdown() sets DrainingIngress first
    auto result = system.shutdown(opts);
    assert(result.has_value());
    assert(system.shutdown_phase() == ShutdownPhase::Stopped);
    assert(!system.is_ready());

    std::printf("PASS: test_shutdown_phase_transitions\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Default shutdown (no args) succeeds
// ═══════════════════════════════════════════════════════════════════════════════

static void test_default_shutdown_succeeds() {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    auto result = system.shutdown();
    assert(result.has_value());

    std::printf("PASS: test_default_shutdown_succeeds\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: Drain config can be set per-actor
// ═══════════════════════════════════════════════════════════════════════════════

static void test_per_actor_drain_config() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* actor = static_cast<test::CountingActor*>(a1.get().get());

    DrainConfig dc{DrainPolicy::DropUserMessages, std::chrono::milliseconds{10000}};
    system.set_drain_config(a1.id(), dc);

    auto configured = actor->as_lifecycle()->drain_config();
    assert(configured.policy == DrainPolicy::DropUserMessages);
    assert(configured.timeout == std::chrono::milliseconds{10000});

    std::printf("PASS: test_per_actor_drain_config\n");
}

int main() {
    test_shutdown_no_user_actors();
    test_shutdown_with_immediate_stop();
    test_shutdown_phase_transitions();
    test_default_shutdown_succeeds();
    test_per_actor_drain_config();
    std::printf("\nAll graceful shutdown system tests passed.\n");
    return 0;
}
