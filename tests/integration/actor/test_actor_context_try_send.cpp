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
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/msg/enqueue_result.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

// Fixture for try_send tests
class ActorContextTrySendTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 0;
        cfg.mailbox.default_capacity = 4;
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

TEST_F(ActorContextTrySendTest, Accepted) {
    ActorContext ctx(sender_, system_.get());

    auto ok = ctx.try_send(target_.address(),
                           TypedMessage(TypeTag::User, StreamBuffer{1}));
    ASSERT_TRUE(ok.get().accepted());
    EXPECT_EQ(ok.get().status, mailbox::DeliveryStatus::Accepted);

    auto* mailbox = system_->get_mailbox(target_.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(received.payload().size(), 1u);
}

TEST_F(ActorContextTrySendTest, FullMailbox) {
    ActorContext ctx(sender_, system_.get());

    // Fill the mailbox (capacity 4)
    for (int i = 0; i < 4; ++i) {
        auto ok = ctx.try_send(target_.address(),
                               TypedMessage(TypeTag::User, StreamBuffer{1}));
        ASSERT_TRUE(ok.get().accepted());
    }

    // Next message should be rejected (mailbox full)
    auto full = ctx.try_send(target_.address(),
                             TypedMessage(TypeTag::User, StreamBuffer{2}));
    EXPECT_FALSE(full.get().accepted());
}

TEST_F(ActorContextTrySendTest, ActorNotFound) {
    ActorContext ctx(sender_, system_.get());

    auto missing_addr = sender_.address();
    missing_addr.id = ActorId{99999};
    auto missing =
        ctx.try_send(missing_addr, TypedMessage(TypeTag::User, StreamBuffer{3}));
    EXPECT_EQ(missing.get().status, mailbox::DeliveryStatus::NoRoute);
}

TEST_F(ActorContextTrySendTest, WithPriority) {
    ActorContext ctx(sender_, system_.get());

    auto ok = ctx.try_send_with_priority(
        target_.address(), TypedMessage(TypeTag::User, StreamBuffer{42}),
        /*priority=*/0, /*deadline_ns=*/INT64_MAX);
    ASSERT_TRUE(ok.get().accepted());

    auto* mailbox = system_->get_mailbox(target_.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(received.payload().size(), 1u);
}

TEST_F(ActorContextTrySendTest, SetsSenderAddress) {
    ActorContext ctx(sender_, system_.get());

    auto ok = ctx.try_send(target_.address(),
                           TypedMessage(TypeTag::User, StreamBuffer{7}));
    ASSERT_TRUE(ok.get().accepted());

    auto* mailbox = system_->get_mailbox(target_.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(received.sender_address().id, sender_.address().id);
}

TEST_F(ActorContextTrySendTest, ExistingSendStillWorks) {
    ActorContext ctx(sender_, system_.get());

    ctx.send(target_.address(), TypedMessage(TypeTag::User, StreamBuffer{55}));

    auto* mailbox = system_->get_mailbox(target_.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    EXPECT_TRUE(popped);
}
