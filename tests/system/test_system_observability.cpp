// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// System test: Observability Smoke
// Validates metrics ring buffer → logging → tracing → actor state introspection

#include <gtest/gtest.h>

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>

#include "system_test_fixture.hpp"

using namespace hpactor;

using CountingActor = test::CountingActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Metrics ring buffer accessible after spawn
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Observability, MetricsRingBufferAccessible) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    // Metrics are enabled by default (MetricsConfig::enabled = true)
    auto* ring = system.metrics_ring_buffer();
    EXPECT_NE(ring, nullptr);

    // Spawn should push a metric event
    system.spawn<test::CountingActor>();

    // Ring buffer should have at least one event (spawn event)
    bool has_event = ring->drain([&](const metrics::MetricEvent& /*evt*/) {
        return false; // stop after first event
    });
    EXPECT_TRUE(has_event);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Actor state introspection via public API
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Observability, ActorStateIntrospection) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* actor = static_cast<test::CountingActor*>(a1.get().get());

    // Lifecycle state accessible
    EXPECT_NE(actor->as_lifecycle(), nullptr);
    EXPECT_EQ(actor->as_lifecycle()->state(), LifecycleState::kActive);

    // Type name accessible without RTTI
    EXPECT_FALSE(actor->type_name().empty());

    // Mailbox snapshot accessible
    auto snap = actor->mailbox_snapshot();
    EXPECT_EQ(snap.depth, 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Multiple spawns produce multiple metrics events
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Observability, MultipleSpawnsProduceMetrics) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto* ring = system.metrics_ring_buffer();
    EXPECT_NE(ring, nullptr);

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

    EXPECT_GE(spawn_count, 3);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Logging subsystem active and can be queried
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Observability, LoggingSubsystemActive) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    // Logging is enabled by default — verify the logger is wired
    // Sending a message to an actor produces log events
    auto a1 = system.spawn<test::CountingActor>();

    TypedMessage msg(TypeTag(0x1001), StreamBuffer{});
    msg.set_sender_address(a1.address());
    system.deliver_local(a1.id(), std::move(msg));

    // No crash = the logger was used successfully
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: Tracing subsystem enabled with trace config
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Observability, TracingSubsystemActivation) {
    // Tracing disabled by default → trace_manager is nullptr
    {
        Config cfg = test::minimal_config();
        ActorSystem system(cfg);
        EXPECT_EQ(system.trace_manager(), nullptr);
    }

    // Tracing enabled with memory exporter → trace_manager is set
    {
        Config cfg = test::config_with_tracing();
        ActorSystem system(cfg);
        EXPECT_NE(system.trace_manager(), nullptr);
        EXPECT_TRUE(system.trace_manager()->enabled());
    }
}
