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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/drain_config.hpp>
#include <hpactor/actor/drain_policy.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/types/types.hpp>

#include <gtest/gtest.h>
#include <memory>

using namespace hpactor;

// Test actor with lifecycle and counting handlers
class DrainTestActor : public EventBasedActor, public LifecycleActor {
  public:
    int user_handler_count = 0;
    int system_handler_count = 0;

    DrainTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            if (static_cast<uint32_t>(msg.type_id()) < 0x1000) {
                system_handler_count++;
            } else {
                user_handler_count++;
            }
        });
    }
};

// Helper: inject a test message into an actor's mailbox
static void inject_message(EventBasedActor* actor, TypeTag tag) {
    auto* mailbox = actor->get_mailbox();
    ASSERT_NE(mailbox, nullptr);

    auto* node = static_cast<TypedMessage*>(mem::allocate(
        mem::RegionType::kMessage, sizeof(TypedMessage), actor->id()));
    new (node) TypedMessage(tag, StreamBuffer{});
    node->set_sender_address(ActorAddress{});

    mailbox->inject_for_test(node);
}

// Helper: enqueue a non-intercepted system message
static void inject_system_message(EventBasedActor* actor) {
    inject_message(actor, TypeTag(0x07));
}

// Helper: get DLQ depth
static uint32_t dlq_depth(ActorSystem& system) {
    auto snapshot = system.dead_letter_snapshot();
    return snapshot.depth;
}

// Helper: spawn and downcast
static DrainTestActor* spawn_test_actor(ActorSystem& system) {
    auto actor_ref = system.spawn<DrainTestActor>();
    auto actor_ptr = actor_ref.get();
    return static_cast<DrainTestActor*>(actor_ptr.get());
}

class DrainPolicyIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
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

TEST_F(DrainPolicyIntegrationTest, DrainPolicyProcessesAllMessages) {
    auto* actor = spawn_test_actor(*system_);

    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    lc->set_drain_config(DrainConfig{DrainPolicy::Drain});

    inject_message(actor, TypeTag(0x1001)); // user
    inject_message(actor, TypeTag(0x1002)); // user
    inject_message(actor, TypeTag(0x1003)); // user
    inject_system_message(actor);           // system

    bool ok = lc->transition(LifecycleState::kDraining);
    ASSERT_TRUE(ok);

    // Process messages via receive
    TypedMessage msg;
    auto* mailbox = actor->get_mailbox();
    while (mailbox->try_pop(msg)) {
        actor->receive(msg);
        if (lc->state() == LifecycleState::kStopped)
            break;
    }

    EXPECT_EQ(actor->user_handler_count, 3);
    EXPECT_EQ(actor->system_handler_count, 1);
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
}

TEST_F(DrainPolicyIntegrationTest, DropUserMessagesDeadlettersUserKeepsSystem) {
    auto* actor = spawn_test_actor(*system_);

    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    lc->set_drain_config(DrainConfig{DrainPolicy::DropUserMessages});

    inject_message(actor, TypeTag(0x1001)); // user
    inject_message(actor, TypeTag(0x1002)); // user
    inject_system_message(actor);           // system

    bool ok = lc->transition(LifecycleState::kDraining);
    ASSERT_TRUE(ok);

    // Process messages via receive
    TypedMessage msg;
    auto* mailbox = actor->get_mailbox();
    while (mailbox->try_pop(msg)) {
        actor->receive(msg);
        if (lc->state() == LifecycleState::kStopped)
            break;
    }

    EXPECT_EQ(actor->system_handler_count, 1);
    EXPECT_EQ(actor->user_handler_count, 0);
    EXPECT_EQ(dlq_depth(*system_), 2u);
}

TEST_F(DrainPolicyIntegrationTest, ImmediateStopDeadlettersAll) {
    auto* actor = spawn_test_actor(*system_);

    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    lc->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});

    inject_message(actor, TypeTag(0x1001)); // user
    inject_message(actor, TypeTag(0x1002)); // user
    inject_system_message(actor);           // system

    // Call drain_all_immediate()
    actor->drain_all_immediate();

    EXPECT_EQ(actor->user_handler_count, 0);
    EXPECT_EQ(actor->system_handler_count, 0);
    EXPECT_EQ(dlq_depth(*system_), 3u);
}

TEST_F(DrainPolicyIntegrationTest, DeferredPolicyFallsBackToDrain) {
    auto* actor = spawn_test_actor(*system_);

    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    lc->set_drain_config(DrainConfig{DrainPolicy::SnapshotAndStop});

    inject_message(actor, TypeTag(0x1001)); // user

    bool ok = lc->transition(LifecycleState::kDraining);
    ASSERT_TRUE(ok);

    // Process message via receive — drain_one should change policy to Drain
    TypedMessage msg;
    auto* mailbox = actor->get_mailbox();
    bool found = mailbox->try_pop(msg);
    ASSERT_TRUE(found);
    actor->receive(msg);

    EXPECT_EQ(lc->drain_config().policy, DrainPolicy::Drain);
    EXPECT_EQ(actor->user_handler_count, 1);
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
}