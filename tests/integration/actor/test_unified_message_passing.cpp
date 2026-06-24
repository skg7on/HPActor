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
#include <hpactor/actor/actor_ref_cache.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <gtest/gtest.h>
#include <string>

using namespace hpactor;

// Fixture for unified message passing tests
class UnifiedMessagePassingTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config;
        config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        config.scheduler_threads = 0;
        system_ = std::make_unique<ActorSystem>(config);
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

TEST_F(UnifiedMessagePassingTest, DeliverRemoteBridge) {
    auto target = system_->spawn<EventBasedActor>();

    net::WireFrame frame;
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_sender(),
                  ActorAddress{endpoint_ops::parse_endpoint("10.0.0.1:9999"),
                               ActorType{1}, ActorId{99}, 0});
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_receiver(),
                  target.address());
    frame.pb_envelope.mutable_data_frame()->set_type_tag(
        static_cast<uint32_t>(TypeTag::User));
    frame.pb_envelope.mutable_data_frame()->set_payload(
        reinterpret_cast<const char*>(StreamBuffer{1, 3, 3, 7}.data()), 4u);

    system_->deliver_remote(frame);

    auto* mailbox = system_->get_mailbox(target.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(received.type_id(), TypeTag::User);
    EXPECT_EQ(received.payload().size(), 4u);
    EXPECT_EQ(received.sender_address().id, ActorId{99});
}

TEST_F(UnifiedMessagePassingTest, SendReplyLoop) {
    auto alice = system_->spawn<EventBasedActor>();
    auto bob = system_->spawn<EventBasedActor>();

    // Alice sends to Bob
    ActorContext alice_ctx(alice, system_.get());
    TypedMessage msg(TypeTag::User, StreamBuffer{42});
    ActorRef bob_ref(bob);
    alice_ctx.send(bob_ref, std::move(msg));

    // Bob receives
    auto* bob_mailbox = system_->get_mailbox(bob.address().id);
    ASSERT_NE(bob_mailbox, nullptr);
    TypedMessage received;
    bool popped = bob_mailbox->try_pop(received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(received.sender_address().id, alice.address().id);

    // Bob replies
    ActorContext bob_ctx(bob, system_.get());
    bob_ctx.set_current_sender(received.sender_address());
    TypedMessage reply_msg(TypeTag::User, StreamBuffer{24});
    bob_ctx.reply(std::move(reply_msg));

    // Alice receives reply
    auto* alice_mailbox = system_->get_mailbox(alice.address().id);
    ASSERT_NE(alice_mailbox, nullptr);
    TypedMessage reply_received;
    popped = alice_mailbox->try_pop(reply_received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(reply_received.sender_address().id, bob.address().id);
    EXPECT_EQ(reply_received.payload()[0], 24);
}

TEST_F(UnifiedMessagePassingTest, ReplyWithError) {
    auto alice = system_->spawn<EventBasedActor>();
    auto bob = system_->spawn<EventBasedActor>();

    ActorContext bob_ctx(bob, system_.get());
    bob_ctx.set_current_sender(alice.address());
    bob_ctx.reply_with_error(error(42, "something went wrong"));

    auto* alice_mailbox = system_->get_mailbox(alice.address().id);
    ASSERT_NE(alice_mailbox, nullptr);
    TypedMessage reply_received;
    bool popped = alice_mailbox->try_pop(reply_received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(reply_received.type_id(), TypeTag::ErrorMsg);
    EXPECT_GE(reply_received.payload().size(), 4u);
    EXPECT_EQ(reply_received.payload()[0], 0);
    EXPECT_EQ(reply_received.payload()[1], 0);
    EXPECT_EQ(reply_received.payload()[2], 0);
    EXPECT_EQ(reply_received.payload()[3], 42);
    EXPECT_EQ(reply_received.sender_address().id, bob.address().id);
}

TEST_F(UnifiedMessagePassingTest, SendToSelf) {
    auto actor = system_->spawn<EventBasedActor>();
    ActorContext ctx(actor, system_.get());

    TypedMessage msg(TypeTag::User, StreamBuffer{7});
    ActorRef self_ref(actor);
    ctx.send(self_ref, std::move(msg));

    auto* mailbox = system_->get_mailbox(actor.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(received.sender_address().id, actor.address().id);
}
