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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/delivery_mode.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/frame.hpp>

#include <chrono>
#include <gtest/gtest.h>

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
    auto result = ctx.try_send(
        target_.address(), TypedMessage(TypeTag::User, StreamBuffer{1}), opts);
    EXPECT_TRUE(result.get().accepted());
    EXPECT_EQ(result.get().status, DeliveryStatus::Accepted);
}

TEST_F(DeliverySemanticsTest, DefaultOptionsBestEffort) {
    // Default-constructed DeliveryOptions must be BestEffort.
    ActorContext ctx(sender_, system_.get());

    auto result = ctx.try_send(target_.address(),
                               TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_TRUE(result.get().accepted());
}

// ── ObservableBestEffort ───────────────────────────────────────────────────

TEST_F(DeliverySemanticsTest, ObservableBestEffortAccepted) {
    ActorContext ctx(sender_, system_.get());

    DeliveryOptions opts;
    opts.delivery_mode = DeliveryMode::ObservableBestEffort;
    auto result = ctx.try_send(
        target_.address(), TypedMessage(TypeTag::User, StreamBuffer{1}), opts);
    EXPECT_TRUE(result.get().accepted());
}

// ── AtLeastOnce ────────────────────────────────────────────────────────────

TEST_F(DeliverySemanticsTest, AtLeastOnceAccepted) {
    ActorContext ctx(sender_, system_.get());

    DeliveryOptions opts;
    opts.delivery_mode = DeliveryMode::AtLeastOnce;
    opts.message_id = 42;
    auto result = ctx.try_send(
        target_.address(), TypedMessage(TypeTag::User, StreamBuffer{1}), opts);
    EXPECT_TRUE(result.get().accepted());
}

TEST_F(DeliverySemanticsTest, AtLeastOnceDuplicateSuppressed) {
    ActorContext ctx(sender_, system_.get());

    DeliveryOptions opts;
    opts.delivery_mode = DeliveryMode::AtLeastOnce;
    opts.message_id = 42;

    // First send → accepted and delivered.
    auto r1 = ctx.try_send(target_.address(),
                           TypedMessage(TypeTag::User, StreamBuffer{1}), opts);
    EXPECT_TRUE(r1.get().accepted());

    // Second send with same message_id → accepted (duplicate suppressed).
    auto r2 = ctx.try_send(target_.address(),
                           TypedMessage(TypeTag::User, StreamBuffer{2}), opts);
    EXPECT_TRUE(r2.get().accepted());
    EXPECT_EQ(r2.get().status, DeliveryStatus::Accepted);

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
    EXPECT_TRUE(r1.get().accepted());

    auto r2 = ctx.try_send(target_.address(),
                           TypedMessage(TypeTag::User, StreamBuffer{2}), opts);
    EXPECT_TRUE(r2.get().accepted());

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
    auto result =
        ctx.try_send(missing_addr, TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_EQ(result.get().status, DeliveryStatus::NoRoute);
    EXPECT_EQ(result.get().failure_reason(), FailureReason::NoRoute);
}

// ── Deadline enforcement ───────────────────────────────────────────────────

TEST_F(DeliverySemanticsTest, ExpiredMessageRejectedAtEnqueue) {
    ActorContext ctx(sender_, system_.get());

    int64_t past_deadline = 1; // 1 ns since epoch → definitely expired

    auto result = ctx.try_send_with_priority(
        target_.address(), TypedMessage(TypeTag::User, StreamBuffer{1}),
        /*priority=*/0, past_deadline,
        DeliveryOptions{false, false, true, DeliveryMode::ObservableBestEffort,
                        0, 0, std::nullopt});

    // Should be rejected because deadline is in the past.
    EXPECT_FALSE(result.get().accepted());
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

// ── Remote delivery round-trip (loopback via deliver_remote) ─────────────

TEST(RemoteDeliveryRoundTripTest, DeliverRemoteAccepted) {
    // System A acts as the "remote" sender.
    Config cfg_a;
    cfg_a.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg_a.scheduler_threads = 0;
    ActorSystem system_a(cfg_a);

    // System B receives the remote message.
    Config cfg_b;
    cfg_b.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg_b.scheduler_threads = 0;
    ActorSystem system_b(cfg_b);

    auto target = system_b.spawn<EventBasedActor>();

    // Construct a wire frame as if sent from system A to system B's actor.
    ActorAddress sender_addr{endpoint_ops::parse_endpoint("127.0.0.1:9001"),
                             ActorType{0}, ActorId{1}, 0};
    net::WireFrame frame;
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_sender(),
                  sender_addr);
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_receiver(),
                  target.address());
    frame.pb_envelope.mutable_data_frame()->set_type_tag(
        static_cast<uint32_t>(TypeTag::User));
    frame.pb_envelope.mutable_data_frame()->set_message_id(1);

    // Deliver the remote frame — this exercises the full remote ingress
    // path and should result in the message being enqueued.
    system_b.deliver_remote(frame);

    // Verify the message was delivered to the target mailbox.
    auto* mailbox = system_b.get_mailbox(target.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage msg;
    EXPECT_TRUE(mailbox->try_pop(msg));

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);
    system_a.shutdown(opts);
    system_b.shutdown(opts);
}

TEST(RemoteDeliveryRoundTripTest, DeliverWithResultToNonexistentActor) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    // Use a non-existent actor id to trigger NoRoute.
    ActorId bad_id{999999};
    auto dr = system.deliver_with_result(
        bad_id, TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_FALSE(dr.ok());
    EXPECT_EQ(dr.status, mailbox::DeliveryStatus::NoRoute);
    EXPECT_TRUE(dr.retryable());

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);
    system.shutdown(opts);
}

TEST(RemoteDeliveryRoundTripTest, DeliverWithResultToLiveActor) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto target = system.spawn<EventBasedActor>();

    auto dr = system.deliver_with_result(
        target.address().id, TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_TRUE(dr.ok());
    EXPECT_EQ(dr.status, mailbox::DeliveryStatus::Accepted);

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);
    system.shutdown(opts);
}
