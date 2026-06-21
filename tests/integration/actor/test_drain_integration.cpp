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
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/drain_config.hpp>
#include <hpactor/actor/lifecycle/drain_policy.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace hpactor;

// Integration test actor with lifecycle and counting handlers
class DrainIntegrationTestActor : public EventBasedActor, public LifecycleActor {
  public:
    int user_handler_count = 0;
    int system_handler_count = 0;

    DrainIntegrationTestActor(ActorContext* ctx, ActorSystem& sys)
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
        return Behavior{[this](TypedMessage& msg) {
            if (static_cast<uint32_t>(msg.type_id()) < 0x1000) {
                system_handler_count++;
            } else {
                user_handler_count++;
            }
        }};
    }
};

// Helper: inject a test message into an actor's mailbox
static void inject_message(EventBasedActor* actor, TypeTag tag) {
    auto* mailbox = actor->get_mailbox();
    ASSERT_NE(mailbox, nullptr);

    static_assert(sizeof(TypedMessage) <= 1024, "TypedMessage must fit in 1024 "
                                                "bytes for stack allocation");

    auto* node = static_cast<TypedMessage*>(mem::allocate(
        mem::RegionType::kMessage, sizeof(TypedMessage), actor->id()));
    new (node) TypedMessage(tag, StreamBuffer{});
    node->set_sender_address(ActorAddress{});

    mailbox->inject_for_test(node);
}

// Helper: enqueue a system message (bypasses system message switch)
static void inject_system_message(EventBasedActor* actor) {
    inject_message(actor, TypeTag(0x07));
}

// Helper: get DLQ depth
static uint32_t dlq_depth(ActorSystem& system) {
    return system.dead_letter_snapshot().depth;
}

// DLQ counts bucket
struct DlqCounts {
    uint32_t total = 0;
    uint32_t drain_policy_drop = 0;
    uint32_t mailbox_closed = 0;
    uint32_t drain_timeout = 0;
    uint32_t other = 0;
};

class DrainIntegrationTest : public ::testing::Test {
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

TEST_F(DrainIntegrationTest, FullShutdownDrainsSpawnTree) {
    auto ref1 = system_->spawn<DrainIntegrationTestActor>();
    auto ref2 = system_->spawn<DrainIntegrationTestActor>();
    auto ref3 = system_->spawn<DrainIntegrationTestActor>();
    auto ref4 = system_->spawn<DrainIntegrationTestActor>();

    auto* a1 = static_cast<DrainIntegrationTestActor*>(ref1.get().get());
    auto* a2 = static_cast<DrainIntegrationTestActor*>(ref2.get().get());
    auto* a3 = static_cast<DrainIntegrationTestActor*>(ref3.get().get());
    auto* a4 = static_cast<DrainIntegrationTestActor*>(ref4.get().get());

    // Use ImmediateStop for deterministic shutdown
    a1->as_lifecycle()->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});
    a2->as_lifecycle()->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});
    a3->as_lifecycle()->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});
    a4->as_lifecycle()->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});

    // Enqueue 3 user messages to each actor
    for (auto* actor : {a1, a2, a3, a4}) {
        inject_message(actor, TypeTag(0x1001));
        inject_message(actor, TypeTag(0x1002));
        inject_message(actor, TypeTag(0x1003));
    }

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);

    auto result = system_->shutdown(opts);
    ASSERT_TRUE(result.has_value());

    // All 4 actors should reach kStopped
    for (auto* actor : {a1, a2, a3, a4}) {
        EXPECT_EQ(actor->as_lifecycle()->state(), LifecycleState::kStopped);
    }

    // All 12 messages should be in DLQ (ImmediateStop dead-letters everything)
    EXPECT_EQ(dlq_depth(*system_), 12u);

    // Pop all DLQ records and verify they are all MailboxClosed.
    std::vector<ActorId> dlq_targets;
    {
        mailbox::DeadLetterRecord record;
        while (system_->pop_dead_letter(record)) {
            EXPECT_EQ(record.reason, mailbox::DeadLetterReason::MailboxClosed);
            dlq_targets.push_back(record.target.id);
        }
    }
    EXPECT_EQ(dlq_targets.size(), 12u);

    // Verify each of the 4 actors appears exactly 3 times
    int a1_hits = 0, a2_hits = 0, a3_hits = 0, a4_hits = 0;
    for (auto& target_id : dlq_targets) {
        if (target_id == a1->id())
            a1_hits++;
        else if (target_id == a2->id())
            a2_hits++;
        else if (target_id == a3->id())
            a3_hits++;
        else if (target_id == a4->id())
            a4_hits++;
    }
    EXPECT_EQ(a1_hits, 3);
    EXPECT_EQ(a2_hits, 3);
    EXPECT_EQ(a3_hits, 3);
    EXPECT_EQ(a4_hits, 3);
}

TEST_F(DrainIntegrationTest, DrainPolicyFlowsEndToEnd) {
    // Spawn 3 actors with different drain policies
    auto ref_a = system_->spawn<DrainIntegrationTestActor>();
    auto* actor_a = static_cast<DrainIntegrationTestActor*>(ref_a.get().get());
    actor_a->as_lifecycle()->set_drain_config(
        DrainConfig{DrainPolicy::Drain, std::chrono::milliseconds{500}});

    auto ref_b = system_->spawn<DrainIntegrationTestActor>();
    auto* actor_b = static_cast<DrainIntegrationTestActor*>(ref_b.get().get());
    actor_b->as_lifecycle()->set_drain_config(DrainConfig{
        DrainPolicy::DropUserMessages, std::chrono::milliseconds{500}});

    auto ref_c = system_->spawn<DrainIntegrationTestActor>();
    auto* actor_c = static_cast<DrainIntegrationTestActor*>(ref_c.get().get());
    actor_c->as_lifecycle()->set_drain_config(
        DrainConfig{DrainPolicy::ImmediateStop});

    // Enqueue messages
    // Actor A: 3 user messages (all should be processed by Drain policy)
    inject_message(actor_a, TypeTag(0x1001));
    inject_message(actor_a, TypeTag(0x1002));
    inject_message(actor_a, TypeTag(0x1003));

    // Actor B: 2 user + 2 system messages
    inject_message(actor_b, TypeTag(0x1001));
    inject_message(actor_b, TypeTag(0x1002));
    inject_system_message(actor_b);
    inject_system_message(actor_b);

    // Actor C: 2 user + 1 system messages
    inject_message(actor_c, TypeTag(0x1001));
    inject_message(actor_c, TypeTag(0x1002));
    inject_system_message(actor_c);

    // Manually drain actor A (Drain policy)
    {
        bool ok = actor_a->as_lifecycle()->transition(LifecycleState::kDraining);
        ASSERT_TRUE(ok);
        TypedMessage msg;
        auto* mailbox = actor_a->get_mailbox();
        while (mailbox->try_pop(msg)) {
            actor_a->receive(msg);
            if (actor_a->as_lifecycle()->state() == LifecycleState::kStopped)
                break;
        }
    }

    // Manually drain actor B (DropUserMessages policy)
    {
        bool ok = actor_b->as_lifecycle()->transition(LifecycleState::kDraining);
        ASSERT_TRUE(ok);
        TypedMessage msg;
        auto* mailbox = actor_b->get_mailbox();
        while (mailbox->try_pop(msg)) {
            actor_b->receive(msg);
            if (actor_b->as_lifecycle()->state() == LifecycleState::kStopped)
                break;
        }
    }

    // Verify per-actor handler counts before shutdown
    EXPECT_EQ(actor_a->user_handler_count, 3);
    EXPECT_EQ(actor_a->system_handler_count, 0);

    EXPECT_EQ(actor_b->system_handler_count, 2);
    EXPECT_EQ(actor_b->user_handler_count, 0);

    EXPECT_EQ(actor_c->user_handler_count, 0);
    EXPECT_EQ(actor_c->system_handler_count, 0);

    // Call shutdown — actor C (ImmediateStop) is handled synchronously
    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);

    auto result = system_->shutdown(opts);
    ASSERT_TRUE(result.has_value());

    // All 3 actors must reach kStopped
    EXPECT_EQ(actor_a->as_lifecycle()->state(), LifecycleState::kStopped);
    EXPECT_EQ(actor_b->as_lifecycle()->state(), LifecycleState::kStopped);
    EXPECT_EQ(actor_c->as_lifecycle()->state(), LifecycleState::kStopped);

    // Actor C must not have processed any messages (ImmediateStop)
    EXPECT_EQ(actor_c->user_handler_count, 0);
    EXPECT_EQ(actor_c->system_handler_count, 0);

    // Pop all DLQ records in a single pass and bucket by actor + reason
    DlqCounts counts_a;
    DlqCounts counts_b;
    DlqCounts counts_c;
    {
        mailbox::DeadLetterRecord record;
        while (system_->pop_dead_letter(record)) {
            DlqCounts* bucket = nullptr;
            if (record.target.id == actor_a->id())
                bucket = &counts_a;
            else if (record.target.id == actor_b->id())
                bucket = &counts_b;
            else if (record.target.id == actor_c->id())
                bucket = &counts_c;
            else
                continue;
            bucket->total++;
            switch (record.reason) {
                case mailbox::DeadLetterReason::DrainPolicyDrop:
                    bucket->drain_policy_drop++;
                    break;
                case mailbox::DeadLetterReason::MailboxClosed:
                    bucket->mailbox_closed++;
                    break;
                case mailbox::DeadLetterReason::DrainTimeout:
                    bucket->drain_timeout++;
                    break;
                default:
                    bucket->other++;
                    break;
            }
        }
    }

    // Actor A (Drain): no records in DLQ
    EXPECT_EQ(counts_a.total, 0u);

    // Actor B (DropUserMessages): 2 user messages dead-lettered
    EXPECT_EQ(counts_b.total, 2u);
    EXPECT_EQ(counts_b.drain_policy_drop, 2u);
    EXPECT_EQ(counts_b.mailbox_closed, 0u);

    // Actor C (ImmediateStop): all 3 messages dead-lettered
    EXPECT_EQ(counts_c.total, 3u);
    EXPECT_EQ(counts_c.mailbox_closed, 3u);
}