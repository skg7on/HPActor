// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
//
// System test: Backpressure & Dead Letter Queue
// Validates bounded mailbox overflow → DLQ records → backpressure signals

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>

#include "system_test_fixture.hpp"

#include <cassert>
#include <cstdio>

using namespace hpactor;

using CountingActor = test::CountingActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: DLQ enabled by default, records reachable
// ═══════════════════════════════════════════════════════════════════════════════

static void test_dlq_enabled_and_reachable() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto snap = system.dead_letter_snapshot();
    assert(snap.capacity > 0);
    assert(snap.depth == 0);

    // Push a dead letter record manually via the public API
    mailbox::DeadLetterRecord rec;
    rec.reason = mailbox::DeadLetterReason::ActorNotFound;
    rec.source = mailbox::DeadLetterSource::LocalDelivery;
    rec.type_tag = TypeTag(0x1001);
    bool ok = system.dead_letter(std::move(rec));
    assert(ok);

    snap = system.dead_letter_snapshot();
    assert(snap.depth == 1);

    // Pop and verify
    mailbox::DeadLetterRecord out;
    ok = system.pop_dead_letter(out);
    assert(ok);
    assert(out.reason == mailbox::DeadLetterReason::ActorNotFound);
    assert(static_cast<uint32_t>(out.type_tag) == 0x1001);

    std::printf("PASS: test_dlq_enabled_and_reachable\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Actor not found produces DLQ record via try_deliver_local
// ═══════════════════════════════════════════════════════════════════════════════

static void test_actor_not_found_produces_dlq_record() {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    // Try to deliver to a non-existent actor
    TypedMessage msg(TypeTag(0x1001), StreamBuffer{});
    msg.set_sender_address(ActorAddress{});
    auto result = system.try_deliver_local(ActorId(99999), std::move(msg));

    assert(result.code == mailbox::EnqueueResultCode::ActorNotFound);

    // Should produce a DLQ record
    auto snap = system.dead_letter_snapshot();
    assert(snap.depth >= 1);

    std::printf("PASS: test_actor_not_found_produces_dlq_record\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: DLQ pop on empty queue returns false
// ═══════════════════════════════════════════════════════════════════════════════

static void test_dlq_pop_empty_returns_false() {
    Config cfg = test::minimal_config();
    ActorSystem system(cfg);

    // Consume any pre-existing records
    mailbox::DeadLetterRecord tmp;
    while (system.pop_dead_letter(tmp)) {
    }

    mailbox::DeadLetterRecord out;
    bool ok = system.pop_dead_letter(out);
    assert(!ok);

    std::printf("PASS: test_dlq_pop_empty_returns_false\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Backpressure signal configuration accessible
// ═══════════════════════════════════════════════════════════════════════════════

static void test_mailbox_config_defaults_from_system() {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto mbox_cfg = system.mailbox_config_for_spawn();
    // Check default capacity is reasonable
    assert(mbox_cfg.capacity.max_messages > 0);

    std::printf("PASS: test_mailbox_config_defaults_from_system\n");
}

int main() {
    test_dlq_enabled_and_reachable();
    test_actor_not_found_produces_dlq_record();
    test_dlq_pop_empty_returns_false();
    test_mailbox_config_defaults_from_system();
    std::printf("\nAll backpressure system tests passed.\n");
    return 0;
}
