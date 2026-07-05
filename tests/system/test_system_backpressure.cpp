// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// System test: Backpressure & Dead Letter Queue
// Validates bounded mailbox overflow → DLQ records → backpressure signals

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/config/actor_factory_registry.hpp>

#include "system_test_fixture.hpp"

using namespace hpactor;

using CountingActor = test::CountingActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: DLQ enabled by default, records reachable
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Backpressure, DlqEnabledAndReachable) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto snap = system.dead_letter_snapshot();
    EXPECT_GT(snap.capacity, 0);
    EXPECT_EQ(snap.depth, 0);

    // Push a dead letter record manually via the public API
    mailbox::DeadLetterRecord rec;
    rec.reason = mailbox::DeadLetterReason::ActorNotFound;
    rec.source = mailbox::DeadLetterSource::LocalDelivery;
    rec.type_tag = TypeTag(0x1001);
    bool ok = system.dead_letter(std::move(rec));
    EXPECT_TRUE(ok);

    snap = system.dead_letter_snapshot();
    EXPECT_EQ(snap.depth, 1);

    // Pop and verify
    mailbox::DeadLetterRecord out;
    ok = system.pop_dead_letter(out);
    EXPECT_TRUE(ok);
    EXPECT_EQ(out.reason, mailbox::DeadLetterReason::ActorNotFound);
    EXPECT_EQ(static_cast<uint32_t>(out.type_tag), 0x1001u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Actor not found produces DLQ record via try_deliver_local
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Backpressure, ActorNotFoundProducesDlqRecord) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    // Try to deliver to a non-existent actor
    TypedMessage msg(TypeTag(0x1001), StreamBuffer{});
    msg.set_sender_address(ActorAddress{});
    auto result = system.try_deliver_local(ActorId(99999), std::move(msg));

    EXPECT_EQ(result.code, mailbox::EnqueueResultCode::ActorNotFound);

    // Should produce a DLQ record
    auto snap = system.dead_letter_snapshot();
    EXPECT_GE(snap.depth, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: DLQ pop on empty queue returns false
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Backpressure, DlqPopEmptyReturnsFalse) {
    Config cfg = test::minimal_config();
    ActorSystem system(cfg);

    // Consume any pre-existing records
    mailbox::DeadLetterRecord tmp;
    while (system.pop_dead_letter(tmp)) {
    }

    mailbox::DeadLetterRecord out;
    bool ok = system.pop_dead_letter(out);
    EXPECT_FALSE(ok);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Backpressure signal configuration accessible
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Backpressure, MailboxConfigDefaultsFromSystem) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto mbox_cfg = system.mailbox_config_for_spawn();
    // Check default capacity is reasonable
    EXPECT_GT(mbox_cfg.capacity.max_messages, 0);
}