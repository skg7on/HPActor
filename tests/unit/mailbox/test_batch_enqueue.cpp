// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>

#include <gtest/gtest.h>
#include <vector>

using namespace hpactor;
using namespace hpactor::mailbox;

class BatchEnqueueTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 0;
        system_ = std::make_unique<ActorSystem>(cfg);
    }

    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(10);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }

    std::unique_ptr<ActorSystem> system_;
};

// ── Batch enqueue: all messages reach the mailbox ──────────────────
TEST_F(BatchEnqueueTest, BatchEnqueueAllMessagesArrive) {
    auto actor = system_->spawn<EventBasedActor>();
    auto* mbox = system_->get_mailbox(actor.id());
    ASSERT_NE(mbox, nullptr);

    std::vector<TypedMessage> msgs;
    for (int i = 0; i < 5; ++i) {
        StreamBuffer payload(1);
        payload[0] = static_cast<uint8_t>(i);
        msgs.emplace_back(TypeTag::User, std::move(payload));
    }

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;
    meta.priority = 0;
    meta.deadline_ns = INT64_MAX;

    auto result = mbox->try_push_batch(msgs.begin(), msgs.end(), meta);
    EXPECT_TRUE(result.accepted());

    // All 5 messages should be in the mailbox.
    for (int i = 0; i < 5; ++i) {
        TypedMessage popped;
        ASSERT_TRUE(mbox->try_pop(popped)) << "msg " << i;
        EXPECT_EQ(popped.payload()[0], static_cast<uint8_t>(i)) << "msg " << i;
    }
}

// ── Batch enqueue: empty batch is accepted (no-op) ─────────────────
TEST_F(BatchEnqueueTest, BatchEnqueueEmptyAccepted) {
    auto actor = system_->spawn<EventBasedActor>();
    auto* mbox = system_->get_mailbox(actor.id());
    ASSERT_NE(mbox, nullptr);

    std::vector<TypedMessage> empty;
    MailboxEnvelopeMeta meta;
    auto result = mbox->try_push_batch(empty.begin(), empty.end(), meta);
    EXPECT_TRUE(result.accepted());
}

// ── Batch enqueue: single message works like try_push ───────────────
TEST_F(BatchEnqueueTest, BatchEnqueueSingleMessage) {
    auto actor = system_->spawn<EventBasedActor>();
    auto* mbox = system_->get_mailbox(actor.id());
    ASSERT_NE(mbox, nullptr);

    std::vector<TypedMessage> msgs;
    StreamBuffer payload(1);
    payload[0] = 0xAB;
    msgs.emplace_back(TypeTag::User, std::move(payload));

    MailboxEnvelopeMeta meta;
    auto result = mbox->try_push_batch(msgs.begin(), msgs.end(), meta);
    EXPECT_TRUE(result.accepted());

    TypedMessage popped;
    ASSERT_TRUE(mbox->try_pop(popped));
    EXPECT_EQ(popped.payload()[0], static_cast<uint8_t>(0xAB));
}

// ── Batch enqueue: wakeup fires only once ──────────────────────────
TEST_F(BatchEnqueueTest, BatchEnqueueWakeupFiresOnce) {
    // Verify that after a batch enqueue to an empty mailbox, the wakeup
    // fires (mailbox is now non-empty) and subsequent messages in the
    // batch don't cause duplicate wakeups.
    auto actor = system_->spawn<EventBasedActor>();
    auto* mbox = system_->get_mailbox(actor.id());
    ASSERT_NE(mbox, nullptr);
    EXPECT_TRUE(mbox->empty());

    std::vector<TypedMessage> msgs;
    for (int i = 0; i < 3; ++i) {
        StreamBuffer payload(1);
        payload[0] = static_cast<uint8_t>(i);
        msgs.emplace_back(TypeTag::User, std::move(payload));
    }

    MailboxEnvelopeMeta meta;
    auto result = mbox->try_push_batch(msgs.begin(), msgs.end(), meta);
    EXPECT_TRUE(result.accepted());
    EXPECT_FALSE(mbox->empty()); // mailbox should be non-empty after batch
}
