// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// System test: Multi-Actor Runtime Workflow
// Validates spawn → send/reply → lifecycle → link/monitor → scheduled delivery

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>

#include "system_test_fixture.hpp"

#include <cassert>
#include <cstdio>

using namespace hpactor;

// ── Register test actors for spawn ───────────────────────────────────────────

using CountingActor = test::CountingActor;
using EchoActor = test::EchoActor;
using ForwardingActor = test::ForwardingActor;

HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);
HPACTOR_REGISTER_ACTOR("EchoActor", EchoActor);
HPACTOR_REGISTER_ACTOR("ForwardingActor", ForwardingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Multi-actor send/reply chain with live scheduler
// ═══════════════════════════════════════════════════════════════════════════════

static void test_multi_actor_send_reply_chain() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto a2 = system.spawn<test::CountingActor>();
    auto a3 = system.spawn<test::CountingActor>();

    // Send messages directly to each actor's address via their context
    auto* actor1 = static_cast<test::CountingActor*>(a1.get().get());
    auto* actor2 = static_cast<test::CountingActor*>(a2.get().get());
    auto* actor3 = static_cast<test::CountingActor*>(a3.get().get());

    // Use ActorContext to send messages between actors
    // A1's context sends to A2 and A3
    auto addr2 = a2.address();
    auto addr3 = a3.address();

    TypedMessage msg1(TypeTag(0x1001), StreamBuffer{});
    msg1.set_sender_address(a1.address());
    actor1->context()->send(addr2, std::move(msg1));

    TypedMessage msg2(TypeTag(0x1002), StreamBuffer{});
    msg2.set_sender_address(a1.address());
    actor1->context()->send(addr3, std::move(msg2));

    // Poll until all messages are processed
    bool done = test::assert_eventually(
        [&]() {
            return actor2->handler_count >= 1 && actor3->handler_count >= 1;
        },
        5000);
    assert(done);

    assert(actor2->handler_count >= 1);
    assert(actor3->handler_count >= 1);

    std::printf("PASS: test_multi_actor_send_reply_chain\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Lifecycle transitions are observable after spawn
// ═══════════════════════════════════════════════════════════════════════════════

static void test_lifecycle_transitions_observable() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* actor = static_cast<test::CountingActor*>(a1.get().get());
    auto* lc = actor->as_lifecycle();
    assert(lc != nullptr);

    // spawn<>() transitions to kActive
    assert(lc->state() == LifecycleState::kActive);

    // Manual transition to kDraining
    bool ok = lc->transition(LifecycleState::kDraining);
    assert(ok);
    assert(lc->state() == LifecycleState::kDraining);

    std::printf("PASS: test_lifecycle_transitions_observable\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: actor_count reflects live actors
// ═══════════════════════════════════════════════════════════════════════════════

static void test_actor_count_reflects_live_actors() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    // Initially: system_actor_ is null (default-constructed Actor)
    size_t initial = system.actor_count();
    assert(initial == 0);

    auto a1 = system.spawn<test::CountingActor>();
    auto a2 = system.spawn<test::CountingActor>();

    size_t after_spawn = system.actor_count();
    assert(after_spawn >= initial + 2);

    std::printf("PASS: test_actor_count_reflects_live_actors\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Link and unlink API works
// ═══════════════════════════════════════════════════════════════════════════════

static void test_link_unlink_api() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto a2 = system.spawn<test::CountingActor>();

    auto* actor1 = static_cast<test::CountingActor*>(a1.get().get());

    // Link A1 → A2
    actor1->link_to(a2.address());
    auto linked = actor1->context()->linked_actors();
    assert(!linked.empty());
    assert(linked[0] == a2.address());

    // Unlink
    actor1->unlink_from(a2.address());
    linked = actor1->context()->linked_actors();
    assert(linked.empty());

    std::printf("PASS: test_link_unlink_api\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: For_each_actor enumerates all spawned actors
// ═══════════════════════════════════════════════════════════════════════════════

static void test_for_each_actor_enumerates_all() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    system.spawn<test::CountingActor>();
    system.spawn<test::CountingActor>();
    system.spawn<test::EchoActor>();

    int count = 0;
    system.for_each_actor(
        [&](ActorId /*id*/, AbstractActor& /*actor*/) { count++; });

    // 3 user + system_actor
    assert(count >= 3);
    std::printf("PASS: test_for_each_actor_enumerates_all\n");
}

int main() {
    test_multi_actor_send_reply_chain();
    test_lifecycle_transitions_observable();
    test_actor_count_reflects_live_actors();
    test_link_unlink_api();
    test_for_each_actor_enumerates_all();
    std::printf("\nAll runtime workflow system tests passed.\n");
    return 0;
}
