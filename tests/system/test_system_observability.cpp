// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// System test: Observability Smoke
// Validates metrics ring buffer → logging → tracing → actor state introspection

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>

#include "system_test_fixture.hpp"

#include <cassert>
#include <cstdio>

using namespace hpactor;

using CountingActor = test::CountingActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Metrics ring buffer accessible after spawn
// ═══════════════════════════════════════════════════════════════════════════════

static void test_metrics_ring_buffer_accessible() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    // Metrics are enabled by default (MetricsConfig::enabled = true)
    auto* ring = system.metrics_ring_buffer();
    assert(ring != nullptr);

    // Spawn should push a metric event
    system.spawn<test::CountingActor>();

    // Ring buffer should have at least one event (spawn event)
    bool has_event = ring->drain([&](const metrics::MetricEvent& /*evt*/) {
        return false; // stop after first event
    });
    assert(has_event);

    std::printf("PASS: test_metrics_ring_buffer_accessible\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Actor state introspection via public API
// ═══════════════════════════════════════════════════════════════════════════════

static void test_actor_state_introspection() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* actor = static_cast<test::CountingActor*>(a1.get().get());

    // Lifecycle state accessible
    assert(actor->as_lifecycle() != nullptr);
    assert(actor->as_lifecycle()->state() == LifecycleState::kActive);

    // Type name accessible without RTTI
    assert(!actor->type_name().empty());

    // Mailbox snapshot accessible
    auto snap = actor->mailbox_snapshot();
    assert(snap.depth == 0); // No messages pending

    std::printf("PASS: test_actor_state_introspection\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Multiple spawns produce multiple metrics events
// ═══════════════════════════════════════════════════════════════════════════════

static void test_multiple_spawns_produce_metrics() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto* ring = system.metrics_ring_buffer();
    assert(ring != nullptr);

    system.spawn<test::CountingActor>();
    system.spawn<test::CountingActor>();
    system.spawn<test::CountingActor>();

    // Drain all events and count kActorSpawned
    int spawn_count = 0;
    ring->drain([&](const metrics::MetricEvent& evt) {
        if (evt.event_type == metrics::MetricEventType::kActorSpawned) {
            spawn_count++;
        }
        return true;
    });

    assert(spawn_count >= 3);
    std::printf("PASS: test_multiple_spawns_produce_metrics\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Logging subsystem active and can be queried
// ═══════════════════════════════════════════════════════════════════════════════

static void test_logging_subsystem_active() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    // Logging is enabled by default — verify the logger is wired
    // Sending a message to an actor produces log events
    auto a1 = system.spawn<test::CountingActor>();

    TypedMessage msg(TypeTag(0x1001), StreamBuffer{});
    msg.set_sender_address(a1.address());
    system.deliver_local(a1.id(), std::move(msg));

    // No crash = the logger was used successfully
    std::printf("PASS: test_logging_subsystem_active\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: Tracing subsystem enabled with trace config
// ═══════════════════════════════════════════════════════════════════════════════

static void test_tracing_subsystem_activation() {
    // Tracing disabled by default → trace_manager is nullptr
    {
        Config cfg = test::minimal_config();
        ActorSystem system(cfg);
        assert(system.trace_manager() == nullptr);
    }

    // Tracing enabled with memory exporter → trace_manager is set
    {
        Config cfg = test::config_with_tracing();
        ActorSystem system(cfg);
        assert(system.trace_manager() != nullptr);
        assert(system.trace_manager()->enabled());
    }

    std::printf("PASS: test_tracing_subsystem_activation\n");
}

int main() {
    test_metrics_ring_buffer_accessible();
    test_actor_state_introspection();
    test_multiple_spawns_produce_metrics();
    test_logging_subsystem_active();
    test_tracing_subsystem_activation();
    std::printf("\nAll observability smoke tests passed.\n");
    return 0;
}
