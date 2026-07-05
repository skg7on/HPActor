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
#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/actor/lifecycle/passivation_config.hpp>
#include <hpactor/actor/lifecycle/passivation_manager.hpp>
#include <hpactor/actor/lifecycle/quarantine_reason.hpp>
#include <hpactor/actor/spawn/spawn_receiver.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/supervision.hpp>
#include <hpactor/types/types.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace hpactor;

// =============================================================================
// ActorIntegrationFinalTest — fixture with scheduler_threads=0 for
// deterministic message injection and synchronous receive() calls.
// =============================================================================
class ActorIntegrationFinalTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.scheduler_threads = 0;
        cfg.enable_network = false;
        cfg.cli.enabled = false;
        cfg.tracing.enabled = false;
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

// =============================================================================
// Test 1: Actor spawn with different configurations
// =============================================================================

TEST_F(ActorIntegrationFinalTest, SpawnEventBasedActor) {
    auto actor = system_->spawn<EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(actor));
    EXPECT_NE(actor.get(), nullptr);
    EXPECT_GT(actor.id().value(), 0u);
}

TEST_F(ActorIntegrationFinalTest, SpawnMultipleActorsIncreasesIdCounter) {
    auto a1 = system_->spawn<EventBasedActor>();
    auto a2 = system_->spawn<EventBasedActor>();
    auto a3 = system_->spawn<EventBasedActor>();

    ASSERT_TRUE(static_cast<bool>(a1));
    ASSERT_TRUE(static_cast<bool>(a2));
    ASSERT_TRUE(static_cast<bool>(a3));

    // IDs should be unique
    EXPECT_NE(a1.id(), a2.id());
    EXPECT_NE(a2.id(), a3.id());
    EXPECT_NE(a1.id(), a3.id());
}

// =============================================================================
// Test 2: Actor message send/receive cycle
// =============================================================================

TEST_F(ActorIntegrationFinalTest, FullSendReceiveCycle) {
    auto sender = system_->spawn<EventBasedActor>();
    auto target = system_->spawn<EventBasedActor>();

    ASSERT_TRUE(static_cast<bool>(sender));
    ASSERT_TRUE(static_cast<bool>(target));

    ActorContext ctx(sender, system_.get());

    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3, 4});
    ActorRef target_ref(target);
    ctx.send(target_ref, std::move(msg));

    auto* mailbox = system_->get_mailbox(target.address().id);
    ASSERT_NE(mailbox, nullptr);

    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    ASSERT_TRUE(popped);
    EXPECT_EQ(received.type_id(), TypeTag::User);
    EXPECT_EQ(received.sender_address().id, sender.address().id);
}

// =============================================================================
// Test 3: Actor supervision tree integration
// =============================================================================

namespace {

class FinalTestChild : public EventBasedActor {
  public:
    FinalTestChild(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    int message_count = 0;
};

} // namespace

TEST_F(ActorIntegrationFinalTest, SupervisionTreeWithChildren) {
    OneForOneSupervisor strategy(SupervisionPolicy{});
    std::vector<Actor> children;

    auto child = system_->spawn<FinalTestChild>();
    ASSERT_TRUE(static_cast<bool>(child));
    children.push_back(child);

    SupervisorActor supervisor(nullptr, *system_, strategy, std::move(children));
    // Verify the supervisor constructed successfully with children
    SUCCEED();
}

// =============================================================================
// Test 4: Actor lifecycle state transitions
// =============================================================================

class LifecycleTestActor : public EventBasedActor, public LifecycleActor {
  public:
    LifecycleTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }
};

TEST_F(ActorIntegrationFinalTest, ActorLifecycleStateTransitions) {
    auto actor = system_->spawn<LifecycleTestActor>();
    ASSERT_TRUE(static_cast<bool>(actor));

    auto* lc = actor.get()->as_lifecycle();
    ASSERT_NE(lc, nullptr);

    // After spawn, the actor may already be in kActive (spawn transitions it)
    // Verify at least one valid state transition chain works
    LifecycleState current = lc->state();
    EXPECT_TRUE(current == LifecycleState::kStarting ||
                current == LifecycleState::kActive);

    // If not already active, transition to active
    if (current == LifecycleState::kStarting) {
        lc->transition(LifecycleState::kActive);
    }
    EXPECT_EQ(lc->state(), LifecycleState::kActive);

    // Transition to draining
    bool drained = lc->transition(LifecycleState::kDraining);
    EXPECT_TRUE(drained);
    EXPECT_EQ(lc->state(), LifecycleState::kDraining);

    // Transition to stopping
    bool stopping = lc->transition(LifecycleState::kStopping);
    EXPECT_TRUE(stopping);
    EXPECT_EQ(lc->state(), LifecycleState::kStopping);

    // Transition to stopped
    bool stopped = lc->transition(LifecycleState::kStopped);
    EXPECT_TRUE(stopped);
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
}

TEST_F(ActorIntegrationFinalTest, QuarantinedActorStateIsCorrect) {
    auto actor = system_->spawn<LifecycleTestActor>();
    auto* lc = actor.get()->as_lifecycle();

    lc->transition(LifecycleState::kActive);
    lc->transition_to_quarantined(QuarantineReason::CircuitBreakerTrip);
    ASSERT_TRUE(lc->is_quarantined());
    EXPECT_EQ(lc->quarantine_reason(), QuarantineReason::CircuitBreakerTrip);
}

// =============================================================================
// Test 5: Actor proxy remote send
// =============================================================================

TEST_F(ActorIntegrationFinalTest, ActorProxyRemoteRefConstruction) {
    auto ep = endpoint_ops::parse_endpoint("10.0.0.1:12345");
    ActorAddress remote_addr{ep, ActorType{1}, ActorId{42}, 0};

    ActorProxy proxy(remote_addr, static_cast<net::Transport*>(nullptr));
    ActorRef remote_ref(std::move(proxy));

    EXPECT_FALSE(remote_ref.is_local());
    EXPECT_EQ(remote_ref.address().id, ActorId{42});
    EXPECT_EQ(remote_ref.address(), remote_addr);
}

TEST_F(ActorIntegrationFinalTest, ActorProxyRemoteSendWithNullTransport) {
    auto ep = endpoint_ops::parse_endpoint("10.0.0.2:12346");
    ActorAddress remote_addr{ep, ActorType{2}, ActorId{100}, 1};

    ActorProxy proxy(remote_addr, static_cast<net::Transport*>(nullptr));
    ActorRef remote_ref(std::move(proxy));

    // Sending through a null transport proxy should not crash
    // ActorRef::send() requires a target address and message
    auto target_ep = endpoint_ops::parse_endpoint("10.0.0.3:12347");
    ActorAddress target{target_ep, ActorType{3}, ActorId{200}, 0};
    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    EXPECT_NO_FATAL_FAILURE(remote_ref.send(target, std::move(msg)));
}

// =============================================================================
// Test 6: Actor registry operations
// =============================================================================

TEST_F(ActorIntegrationFinalTest, RegisterAndResolveNamedActor) {
    auto actor = system_->spawn<EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(actor));

    system_->register_actor("final-test-worker", actor);

    auto resolved = system_->resolve_actor("final-test-worker");
    EXPECT_TRUE(static_cast<bool>(resolved));
    EXPECT_EQ(resolved.id(), actor.id());
    EXPECT_EQ(resolved.address(), actor.address());
}

TEST_F(ActorIntegrationFinalTest, ResolveUnknownActorReturnsInvalid) {
    auto resolved = system_->resolve_actor("nonexistent-actor");
    EXPECT_FALSE(static_cast<bool>(resolved));
}

TEST_F(ActorIntegrationFinalTest, RegisterDuplicateNameKeepsFirst) {
    auto a1 = system_->spawn<EventBasedActor>();
    auto a2 = system_->spawn<EventBasedActor>();

    system_->register_actor("overwrite-test", a1);
    system_->register_actor("overwrite-test", a2);

    auto resolved = system_->resolve_actor("overwrite-test");
    EXPECT_TRUE(static_cast<bool>(resolved));
    // First registration wins; second is ignored
    EXPECT_EQ(resolved.id(), a1.id());
}

// =============================================================================
// Test 7: Actor system actor detection
// =============================================================================

class FinalSystemActor : public EventBasedActor {
  public:
    FinalSystemActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    bool is_system_actor() const override {
        return true;
    }
};

TEST_F(ActorIntegrationFinalTest, SystemActorDetection) {
    auto user_actor = system_->spawn<EventBasedActor>();
    auto sys_actor = system_->spawn<FinalSystemActor>();

    EXPECT_FALSE(user_actor.get()->is_system_actor());
    EXPECT_TRUE(sys_actor.get()->is_system_actor());
}

// =============================================================================
// Test 8: SpawnReceiver integration
// =============================================================================

namespace {
struct StubTransport : public net::Transport {
    TransportSendResult try_send(const ActorAddress&, const StreamBuffer&) override {
        return TransportSendResult::Sent;
    }
    void send(const ActorAddress&, const StreamBuffer&) override {}
    net::ConnectionPtr connect(EndPoint, const std::string&, uint16_t) override {
        return nullptr;
    }
    net::ConnectionPtr connect(EndPoint) override {
        return nullptr;
    }
    void listen(uint16_t) override {}
    void stop_listening() override {}
    bool is_connected(EndPoint) const override {
        return false;
    }
    EndPoint endpoint() const override {
        return {};
    }
    void close_connection(EndPoint) override {}
    void set_rpc_handler(rpc_response_handler) override {}
};
} // namespace

TEST_F(ActorIntegrationFinalTest, SpawnReceiverConstructWithTransport) {
    ActorTypeRegistry registry;
    StubTransport transport;
    SpawnReceiver receiver(*system_, registry, &transport);
    auto behavior = receiver.make_behavior();
    (void)behavior;
    SUCCEED();
}

TEST_F(ActorIntegrationFinalTest, SpawnReceiverConstructWithoutTransport) {
    ActorTypeRegistry registry;
    SpawnReceiver receiver(*system_, registry, nullptr);
    auto behavior = receiver.make_behavior();
    (void)behavior;
    SUCCEED();
}

// =============================================================================
// Test 9: Actor state serialization (LifecycleState / QuarantineReason)
// =============================================================================

TEST_F(ActorIntegrationFinalTest, LifecycleStateStringNames) {
    auto actor = system_->spawn<LifecycleTestActor>();
    auto* lc = actor.get()->as_lifecycle();
    ASSERT_NE(lc, nullptr);

    // Actor may already be active after spawn; verify the state_string is valid
    const char* initial = lc->state_string();
    EXPECT_NE(initial, nullptr);
    EXPECT_GT(strlen(initial), 0u);

    // If not already active, transition to active
    if (lc->state() == LifecycleState::kStarting) {
        lc->transition(LifecycleState::kActive);
    }

    EXPECT_STREQ(lc->state_string(), "active");

    // Validate draining state string
    lc->transition(LifecycleState::kDraining);
    EXPECT_STREQ(lc->state_string(), "draining");

    // Validate stopping state string
    lc->transition(LifecycleState::kStopping);
    EXPECT_STREQ(lc->state_string(), "stopping");
}

TEST_F(ActorIntegrationFinalTest, QuarantineReasonToString) {
    EXPECT_STREQ(to_string(QuarantineReason::SupervisionEscalation),
                 "supervision_escalation");
    EXPECT_STREQ(to_string(QuarantineReason::CircuitBreakerTrip),
                 "circuit_breaker_trip");
    EXPECT_STREQ(to_string(QuarantineReason::MailboxPressure), "mailbox_pressure");
    EXPECT_STREQ(to_string(QuarantineReason::OperatorAction), "operator_action");
    EXPECT_STREQ(to_string(QuarantineReason::RecoveryFailure), "recovery_failure");
}

// =============================================================================
// Test 10: Actor passivation/reactivation config
// =============================================================================

TEST_F(ActorIntegrationFinalTest, PassivationConfigDefaults) {
    PassivationConfig config;
    EXPECT_EQ(config.idle_timeout.count(), 0);
    EXPECT_FALSE(config.durable);
    EXPECT_TRUE(config.allow_memory_pressure);
    EXPECT_EQ(config.schema_version, 1u);
}

TEST_F(ActorIntegrationFinalTest, PassivationConfigCustomValues) {
    PassivationConfig config;
    config.idle_timeout = std::chrono::milliseconds(30000);
    config.durable = true;
    config.allow_memory_pressure = false;
    config.schema_version = 2;

    EXPECT_EQ(config.idle_timeout.count(), 30000);
    EXPECT_TRUE(config.durable);
    EXPECT_FALSE(config.allow_memory_pressure);
    EXPECT_EQ(config.schema_version, 2u);
}

TEST_F(ActorIntegrationFinalTest, PassivationManagerConstruct) {
    PassivationConfig config;
    PassivationManager manager(*system_, nullptr, config);
    EXPECT_EQ(manager.durable_store(), nullptr);
    EXPECT_EQ(manager.default_config().idle_timeout.count(), 0);
}

// =============================================================================
// Test 11: ActorContext send/reply edge cases
// =============================================================================

TEST_F(ActorIntegrationFinalTest, ReplyWithNoSenderIsNoOp) {
    auto actor = system_->spawn<EventBasedActor>();
    ActorContext ctx(actor, system_.get());

    // No sender set — reply should be safe
    TypedMessage reply_msg(TypeTag::User, StreamBuffer{99});
    EXPECT_NO_FATAL_FAILURE(ctx.reply(std::move(reply_msg)));
    SUCCEED();
}

TEST_F(ActorIntegrationFinalTest, GetMailboxForUnknownActor) {
    auto* mailbox = system_->get_mailbox(ActorId{99999});
    // Should return nullptr for unknown actor ID
    EXPECT_EQ(mailbox, nullptr);
}

// =============================================================================
// Test 12: Actor context resolution edge cases
// =============================================================================

TEST_F(ActorIntegrationFinalTest, ResolveLocalReturnsLocalRef) {
    auto actor = system_->spawn<EventBasedActor>();
    ActorContext ctx(actor, system_.get());

    ActorRef ref = ctx.resolve(actor.address());
    EXPECT_TRUE(ref);
    EXPECT_TRUE(ref.is_local());
    EXPECT_EQ(ref.address().id, actor.address().id);
}

TEST_F(ActorIntegrationFinalTest, ResolveRemoteReturnsRemoteRef) {
    auto actor = system_->spawn<EventBasedActor>();
    ActorContext ctx(actor, system_.get());

    auto remote_ep = endpoint_ops::parse_endpoint("10.0.0.1:54321");
    ActorAddress remote_addr{remote_ep, ActorType{1}, ActorId{42}, 0};

    ActorRef ref = ctx.resolve(remote_addr);
    EXPECT_TRUE(ref);
    EXPECT_FALSE(ref.is_local());
    EXPECT_EQ(ref.address().id, ActorId{42});
}
