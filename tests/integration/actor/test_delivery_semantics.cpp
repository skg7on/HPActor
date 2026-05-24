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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/delivery_mode.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <gtest/gtest.h>
#include <chrono>

using namespace hpactor;
using namespace hpactor::mailbox;

class DeliverySemanticsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 0;
        cfg.mailbox.default_capacity = 16;
        system_ = std::make_unique<ActorSystem>(cfg);
        sender_ = system_->spawn<EventBasedActor>();
        target_ = system_->spawn<EventBasedActor>();
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
    Actor sender_;
    Actor target_;
};

// ── BestEffort (default) ──────────────────────────────────────────────────

TEST_F(DeliverySemanticsTest, BestEffortDefaultAccepted) {
    ActorContext ctx(sender_, system_.get());

    DeliveryOptions opts;
    opts.delivery_mode = DeliveryMode::BestEffort;
    auto result = ctx.try_send(target_.address(),
                                TypedMessage(TypeTag::User, StreamBuffer{1}),
                                opts);
    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(result.code, EnqueueResultCode::Accepted);
}

TEST_F(DeliverySemanticsTest, DefaultOptionsBestEffort) {
    // Default-constructed DeliveryOptions must be BestEffort.
    ActorContext ctx(sender_, system_.get());

    auto result = ctx.try_send(target_.address(),
                                TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_TRUE(result.accepted());
}

// ── ObservableBestEffort ───────────────────────────────────────────────────

TEST_F(DeliverySemanticsTest, ObservableBestEffortAccepted) {
    ActorContext ctx(sender_, system_.get());

    DeliveryOptions opts;
    opts.delivery_mode = DeliveryMode::ObservableBestEffort;
    auto result = ctx.try_send(target_.address(),
                                TypedMessage(TypeTag::User, StreamBuffer{1}),
                                opts);
    EXPECT_TRUE(result.accepted());
}

// ── AtLeastOnce ────────────────────────────────────────────────────────────

TEST_F(DeliverySemanticsTest, AtLeastOnceAccepted) {
    ActorContext ctx(sender_, system_.get());

    DeliveryOptions opts;
    opts.delivery_mode = DeliveryMode::AtLeastOnce;
    opts.message_id = 42;
    auto result = ctx.try_send(target_.address(),
                                TypedMessage(TypeTag::User, StreamBuffer{1}),
                                opts);
    EXPECT_TRUE(result.accepted());
}

TEST_F(DeliverySemanticsTest, AtLeastOnceDuplicateSuppressed) {
    ActorContext ctx(sender_, system_.get());

    DeliveryOptions opts;
    opts.delivery_mode = DeliveryMode::AtLeastOnce;
    opts.message_id = 42;

    // First send → accepted and delivered.
    auto r1 = ctx.try_send(target_.address(),
                            TypedMessage(TypeTag::User, StreamBuffer{1}), opts);
    EXPECT_TRUE(r1.accepted());

    // Second send with same message_id → accepted (duplicate suppressed).
    auto r2 = ctx.try_send(target_.address(),
                            TypedMessage(TypeTag::User, StreamBuffer{2}), opts);
    EXPECT_TRUE(r2.accepted());
    EXPECT_EQ(r2.code, EnqueueResultCode::Accepted);

    // Verify mailbox has only one message (the first).
    auto* mailbox = system_->get_mailbox(target_.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage msg;
    EXPECT_TRUE(mailbox->try_pop(msg));
    // Only one message was delivered (the duplicate was suppressed).
    EXPECT_FALSE(mailbox->try_pop(msg));
}

TEST_F(DeliverySemanticsTest, AtLeastOnceNoDedupWithoutMessageId) {
    // AtLeastOnce with message_id==0 should skip dedup check.
    ActorContext ctx(sender_, system_.get());

    DeliveryOptions opts;
    opts.delivery_mode = DeliveryMode::AtLeastOnce;
    opts.message_id = 0; // no message id → no dedup

    auto r1 = ctx.try_send(target_.address(),
                            TypedMessage(TypeTag::User, StreamBuffer{1}), opts);
    EXPECT_TRUE(r1.accepted());

    auto r2 = ctx.try_send(target_.address(),
                            TypedMessage(TypeTag::User, StreamBuffer{2}), opts);
    EXPECT_TRUE(r2.accepted());

    // Both messages should be in the mailbox.
    auto* mailbox = system_->get_mailbox(target_.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage msg;
    EXPECT_TRUE(mailbox->try_pop(msg));
    EXPECT_TRUE(mailbox->try_pop(msg));
}

// ── ActorNotFound ──────────────────────────────────────────────────────────

TEST_F(DeliverySemanticsTest, TrySendToDeadActorReturnsActorNotFound) {
    ActorContext ctx(sender_, system_.get());

    // Use a non-existent actor id to trigger ActorNotFound.
    ActorAddress missing_addr = target_.address();
    missing_addr.id = ActorId{99999};
    auto result = ctx.try_send(missing_addr,
                                TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_EQ(result.code, EnqueueResultCode::ActorNotFound);
    EXPECT_EQ(result.failure_reason(), FailureReason::NoRoute);
}

// ── Deadline enforcement ───────────────────────────────────────────────────

TEST_F(DeliverySemanticsTest, ExpiredMessageRejectedAtEnqueue) {
    ActorContext ctx(sender_, system_.get());

    int64_t past_deadline = 1; // 1 ns since epoch → definitely expired

    auto result = ctx.try_send_with_priority(
        target_.address(), TypedMessage(TypeTag::User, StreamBuffer{1}),
        /*priority=*/0, past_deadline,
        DeliveryOptions{.delivery_mode = DeliveryMode::ObservableBestEffort});

    // Should be rejected because deadline is in the past.
    EXPECT_FALSE(result.accepted());
}

// ── Source compatibility ───────────────────────────────────────────────────

TEST_F(DeliverySemanticsTest, SendVoidSourceCompatible) {
    // The fire-and-forget send() must still compile and run.
    ActorContext ctx(sender_, system_.get());
    ctx.send(target_.address(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    // No crash → success. The message should be deliverable.
    auto* mailbox = system_->get_mailbox(target_.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage msg;
    EXPECT_TRUE(mailbox->try_pop(msg));
}

TEST_F(DeliverySemanticsTest, DedupCacheAccessibleFromSystem) {
    auto* cache = system_->dedup_cache();
    ASSERT_NE(cache, nullptr);
    EXPECT_EQ(cache->size(), 0);
}

// ── TypedMessage deadline propagation ──────────────────────────────────────

TEST_F(DeliverySemanticsTest, TypedMessageDeadlineDefaultMax) {
    TypedMessage msg;
    EXPECT_EQ(msg.deadline_ns(), INT64_MAX);
}

TEST_F(DeliverySemanticsTest, TypedMessageDeadlineSetAndGet) {
    TypedMessage msg;
    msg.set_deadline_ns(12345);
    EXPECT_EQ(msg.deadline_ns(), 12345);
}
