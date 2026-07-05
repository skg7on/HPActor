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

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/enqueue_result.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

TEST(DlqHandoffSystemTest, DeadLetterQueueAccessorWorks) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    cfg.cli.enabled = false;
    cfg.dead_letters.enabled = true;
    cfg.dead_letters.capacity = 100;

    ActorSystem system(cfg);
    auto* dlq = system.dead_letter_queue();
    ASSERT_NE(dlq, nullptr);
    EXPECT_TRUE(dlq->config().enabled);

    mailbox::DeadLetterRecord dl;
    dl.reason = mailbox::DeadLetterReason::ActorNotFound;
    dl.source = mailbox::DeadLetterSource::LocalDelivery;
    dl.message_id = 42;
    EXPECT_TRUE(system.dead_letter(std::move(dl)));

    auto snap = system.dead_letter_snapshot();
    EXPECT_EQ(snap.depth, 1u);

    mailbox::DeadLetterRecord out;
    EXPECT_TRUE(system.pop_dead_letter(out));
    EXPECT_EQ(out.message_id, 42u);
}

TEST(DlqHandoffSystemTest, DisabledDlqRejectsRecords) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    cfg.cli.enabled = false;
    cfg.dead_letters.enabled = false;

    ActorSystem system(cfg);
    auto* dlq = system.dead_letter_queue();
    ASSERT_NE(dlq, nullptr);
    EXPECT_FALSE(dlq->config().enabled);

    mailbox::DeadLetterRecord dl;
    dl.message_id = 1;
    EXPECT_FALSE(system.dead_letter(std::move(dl)));

    auto snap = system.dead_letter_snapshot();
    EXPECT_EQ(snap.depth, 0u);
}

TEST(DlqHandoffSystemTest, SnapshotRecordsViaSystem) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    cfg.cli.enabled = false;
    cfg.dead_letters.capacity = 50;

    ActorSystem system(cfg);
    auto* dlq = system.dead_letter_queue();

    for (uint64_t i = 0; i < 3; ++i) {
        mailbox::DeadLetterRecord r;
        r.message_id = i;
        dlq->try_push(std::move(r));
    }

    auto records = dlq->snapshot_records();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].message_id, 0u);
    EXPECT_EQ(records[2].message_id, 2u);

    auto snap = dlq->snapshot();
    EXPECT_EQ(snap.depth, 3u);
}

// ── DLQ routing policy integration ─────────────────────────────────────

// When routing_policy is Never, even a missing-actor failure must not
// produce a dead-letter record.
TEST(DlqRoutingPolicySystemTest, NeverPolicySuppressesDlqOnMissingActor) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    cfg.cli.enabled = false;
    cfg.dead_letters.enabled = true;
    cfg.dead_letters.routing_policy = mailbox::DeadLetterRoutingPolicy::Never;

    ActorSystem system(cfg);
    auto* dlq = system.dead_letter_queue();
    ASSERT_NE(dlq, nullptr);

    // Send to a non-existent actor — BestEffort delivery mode.
    TypedMessage msg(TypeTag{100}, StreamBuffer{1, 2, 3});
    mailbox::DeliveryOptions opts;
    opts.delivery_mode = mailbox::DeliveryMode::BestEffort;
    auto result = system.try_deliver_local(ActorId{9999}, std::move(msg),
                                           /*priority=*/0, INT64_MAX, opts);

    EXPECT_EQ(result.code, mailbox::EnqueueResultCode::ActorNotFound);

    // Policy = Never → no DLQ record should have been created.
    auto snap = system.dead_letter_snapshot();
    EXPECT_EQ(snap.depth, 0u) << "Never policy must suppress DLQ routing for "
                                 "missing actors";
}

// When routing_policy is Always and DLQ is enabled, a missing-actor
// failure must produce a dead-letter record.
TEST(DlqRoutingPolicySystemTest, AlwaysPolicyRoutesMissingActorToDlq) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    cfg.cli.enabled = false;
    cfg.dead_letters.enabled = true;
    cfg.dead_letters.routing_policy = mailbox::DeadLetterRoutingPolicy::Always;

    ActorSystem system(cfg);
    auto* dlq = system.dead_letter_queue();
    ASSERT_NE(dlq, nullptr);

    TypedMessage msg(TypeTag{100}, StreamBuffer{1, 2, 3});
    mailbox::DeliveryOptions opts;
    opts.delivery_mode = mailbox::DeliveryMode::BestEffort;
    auto result = system.try_deliver_local(ActorId{9999}, std::move(msg),
                                           /*priority=*/0, INT64_MAX, opts);

    EXPECT_EQ(result.code, mailbox::EnqueueResultCode::ActorNotFound);

    // Policy = Always → even BestEffort failures create DLQ records.
    auto snap = system.dead_letter_snapshot();
    EXPECT_EQ(snap.depth, 1u) << "Always policy must route missing-actor "
                                 "failures to DLQ";
    EXPECT_EQ(snap.total_pushed, 1u);
}

// TrackedOnly policy should suppress BestEffort but route AtLeastOnce.
TEST(DlqRoutingPolicySystemTest, TrackedOnlyPolicyGatesByDeliveryMode) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    cfg.cli.enabled = false;
    cfg.dead_letters.enabled = true;
    cfg.dead_letters.routing_policy = mailbox::DeadLetterRoutingPolicy::TrackedOnly;

    ActorSystem system(cfg);

    // BestEffort → no DLQ
    {
        TypedMessage msg(TypeTag{100}, StreamBuffer{1, 2, 3});
        mailbox::DeliveryOptions opts;
        opts.delivery_mode = mailbox::DeliveryMode::BestEffort;
        (void)system.try_deliver_local(ActorId{9999}, std::move(msg),
                                       /*priority=*/0, INT64_MAX, opts);
        auto snap = system.dead_letter_snapshot();
        EXPECT_EQ(snap.depth, 0u) << "TrackedOnly must suppress BestEffort DLQ "
                                     "routing";
    }

    // AtLeastOnce → DLQ
    {
        TypedMessage msg(TypeTag{100}, StreamBuffer{4, 5, 6});
        mailbox::DeliveryOptions opts;
        opts.delivery_mode = mailbox::DeliveryMode::AtLeastOnce;
        (void)system.try_deliver_local(ActorId{9999}, std::move(msg),
                                       /*priority=*/0, INT64_MAX, opts);
        auto snap = system.dead_letter_snapshot();
        EXPECT_EQ(snap.depth, 1u) << "TrackedOnly must route AtLeastOnce "
                                     "failures to DLQ";
    }
}
