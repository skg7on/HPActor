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

#include <hpactor/core/actor_system.hpp>
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
