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
#include <hpactor/ref/actor_ref.hpp>

#include <gtest/gtest.h>
#include <type_traits>

using namespace hpactor;

// Fixture for tests that need an ActorSystem with scheduler disabled.
class ActorContextIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 0;
        cfg.enable_network = false;
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

// Tests that don't need an ActorSystem

TEST(ActorContextTest, Children) {
    static_assert(sizeof(ActorContext) > 0, "should not be empty");

    Actor empty_actor;
    ActorContext ctx(empty_actor);

    EXPECT_TRUE(ctx.children().empty());

    Actor child_actor;
    ctx.add_child(child_actor);
    EXPECT_EQ(ctx.children().size(), 1u);

    ctx.remove_child(child_actor);
    EXPECT_TRUE(ctx.children().empty());
}

TEST(ActorContextTest, LinkedActorsEmptyInitially) {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    EXPECT_TRUE(ctx.linked_actors().empty());
}

TEST(ActorContextTest, Monitor) {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    ActorAddress addr{endpoint_ops::parse_endpoint("node1:12345"), ActorType{2},
                      ActorId{3}, 4};
    ctx.monitor(addr);

    auto monitored = ctx.linked_actors();
    (void)monitored;
}

TEST(ActorContextTest, RemoteChildren) {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    EXPECT_TRUE(ctx.remote_children().empty());

    ActorAddress remote_addr{endpoint_ops::parse_endpoint("node2:12345"),
                             ActorType{10}, ActorId{100}, 1};
    ActorProxy proxy(remote_addr, static_cast<net::Transport*>(nullptr));
    ActorRef remote_child(std::move(proxy));

    ctx.add_remote_child(remote_child);
    EXPECT_EQ(ctx.remote_children().size(), 1u);
}

TEST(ActorContextTest, AddRemoveLinked) {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    EXPECT_TRUE(ctx.linked_actors().empty());

    ActorAddress addr1{endpoint_ops::parse_endpoint("127.0.0.1:0"),
                       ActorType{1}, ActorId{10}, 0};
    ctx.add_linked(addr1);
    EXPECT_EQ(ctx.linked_actors().size(), 1u);
    EXPECT_EQ(ctx.linked_actors()[0], addr1);

    // Duplicate add
    ctx.add_linked(addr1);
    EXPECT_EQ(ctx.linked_actors().size(), 2u);

    ctx.remove_linked(addr1);
    EXPECT_EQ(ctx.linked_actors().size(), 1u);

    ctx.remove_linked(addr1);
    EXPECT_TRUE(ctx.linked_actors().empty());

    // Remove non-existent — no-op, no crash
    ctx.remove_linked(addr1);
    EXPECT_TRUE(ctx.linked_actors().empty());
}

TEST(ActorContextTest, AddRemoveMonitored) {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    EXPECT_TRUE(ctx.monitored_actors().empty());

    ActorAddress addr1{endpoint_ops::parse_endpoint("127.0.0.1:0"),
                       ActorType{2}, ActorId{20}, 0};
    ctx.add_monitored(addr1);
    EXPECT_EQ(ctx.monitored_actors().size(), 1u);
    EXPECT_EQ(ctx.monitored_actors()[0], addr1);

    ctx.remove_monitored(addr1);
    EXPECT_TRUE(ctx.monitored_actors().empty());

    // Remove non-existent — no-op
    ctx.remove_monitored(addr1);
    EXPECT_TRUE(ctx.monitored_actors().empty());
}

// Tests using the fixture

TEST_F(ActorContextIntegrationTest, SendWithActorRef) {
    auto actor = system_->spawn<EventBasedActor>();
    ActorContext ctx(actor, system_.get());

    ActorRef target_ref(actor);
    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    ctx.send(target_ref, std::move(msg));

    auto* mailbox = system_->get_mailbox(actor.address().id);
    ASSERT_NE(mailbox, nullptr);
}

TEST_F(ActorContextIntegrationTest, SendSetsSenderAddress) {
    auto sender = system_->spawn<EventBasedActor>();
    auto target = system_->spawn<EventBasedActor>();

    ActorContext ctx(sender, system_.get());

    TypedMessage msg(TypeTag::User, StreamBuffer{42});
    ActorRef target_ref(target);
    ctx.send(target_ref, std::move(msg));

    auto* mailbox = system_->get_mailbox(target.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(received.sender_address().id, sender.address().id);
}

TEST_F(ActorContextIntegrationTest, ResolveLocal) {
    auto actor = system_->spawn<EventBasedActor>();
    ActorContext ctx(actor, system_.get());

    ActorRef ref = ctx.resolve(actor.address());
    EXPECT_TRUE(ref);
    EXPECT_TRUE(ref.is_local());
    EXPECT_EQ(ref.address().id, actor.address().id);
}

TEST_F(ActorContextIntegrationTest, ResolveRemote) {
    auto actor = system_->spawn<EventBasedActor>();
    ActorContext ctx(actor, system_.get());

    auto remote_ep = endpoint_ops::parse_endpoint("10.0.0.1:12345");
    ActorAddress remote_addr{remote_ep, ActorType{1}, ActorId{42}, 0};

    ActorRef ref = ctx.resolve(remote_addr);
    EXPECT_TRUE(ref);
    EXPECT_FALSE(ref.is_local());
    EXPECT_EQ(ref.address().id, ActorId{42});
}

TEST_F(ActorContextIntegrationTest, Reply) {
    auto actor_a = system_->spawn<EventBasedActor>();
    auto actor_b = system_->spawn<EventBasedActor>();

    ActorContext ctx(actor_a, system_.get());
    ctx.set_current_sender(actor_b.address());

    TypedMessage reply_msg(TypeTag::User, StreamBuffer{99});
    ctx.reply(std::move(reply_msg));

    auto* mailbox = system_->get_mailbox(actor_b.address().id);
    ASSERT_NE(mailbox, nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(received.sender_address().id, actor_a.address().id);
}

TEST_F(ActorContextIntegrationTest, ReplyNoSender) {
    auto actor = system_->spawn<EventBasedActor>();
    ActorContext ctx(actor, system_.get());
    // No current_sender_ set — reply should be no-op, not crash
    TypedMessage reply_msg(TypeTag::User, StreamBuffer{99});
    ctx.reply(std::move(reply_msg));
    // Test passes if we reach here without crashing
    SUCCEED();
}
