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
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

// Fixture for actor system backpressure test
class ActorSystemBackpressureTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 0;
        cfg.mailbox.default_capacity = 1;
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

TEST_F(ActorSystemBackpressureTest, TryDeliverLocalAccepted) {
    auto actor = system_->spawn<EventBasedActor>();

    auto ok = system_->try_deliver_local(
        actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    ASSERT_TRUE(ok.accepted());

    TypedMessage popped;
    auto* mailbox = system_->get_mailbox(actor.id());
    ASSERT_NE(mailbox, nullptr);
    ASSERT_TRUE(mailbox->try_pop(popped));
    EXPECT_EQ(popped.payload()[0], 1);
}

TEST_F(ActorSystemBackpressureTest, TryDeliverLocalRejectedWhenFull) {
    auto actor = system_->spawn<EventBasedActor>();

    // Fill the mailbox (capacity 1)
    auto ok = system_->try_deliver_local(
        actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    ASSERT_TRUE(ok.accepted());

    auto full = system_->try_deliver_local(
        actor.id(), TypedMessage(TypeTag::User, StreamBuffer{2}));
    EXPECT_FALSE(full.accepted());
    EXPECT_EQ(full.code, mailbox::EnqueueResultCode::Rejected);
}

TEST_F(ActorSystemBackpressureTest, TryDeliverLocalActorNotFound) {
    auto missing = system_->try_deliver_local(
        ActorId{99999}, TypedMessage(TypeTag::User, StreamBuffer{3}));
    EXPECT_FALSE(missing.accepted());
    EXPECT_EQ(missing.code, mailbox::EnqueueResultCode::ActorNotFound);
}

TEST_F(ActorSystemBackpressureTest, DeadLetterCapturedForActorNotFound) {
    system_->try_deliver_local(ActorId{99999},
                               TypedMessage(TypeTag::User, StreamBuffer{3}));

    auto dl_snap = system_->dead_letter_snapshot();
    EXPECT_EQ(dl_snap.depth, 1u);

    mailbox::DeadLetterRecord dl;
    ASSERT_TRUE(system_->pop_dead_letter(dl));
    EXPECT_EQ(dl.reason, mailbox::DeadLetterReason::ActorNotFound);
    EXPECT_EQ(dl.type_tag, TypeTag::User);

    // No more dead letters
    EXPECT_FALSE(system_->pop_dead_letter(dl));
}
