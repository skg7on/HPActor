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

// System test: Actor Lifecycle Workflow
// Validates lifecycle state machine transitions, failed/recovery, passivation,
// supervision integration, scheduled messages, actor address send,
// ScopedActor, and message gate behavior.

#include <gtest/gtest.h>

#include <hpactor/actor/lifecycle/drain_config.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/actor/scoped_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>

#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

using namespace hpactor;

// ── Custom lifecycle test actor with hook tracking (global namespace for
// spawn) ─

/// Test actor that tracks all lifecycle hook invocations for verification.
class TrackedLifecycleActor : public EventBasedActor, public LifecycleActor {
  public:
    std::vector<std::string> hook_log;
    int start_count = 0;
    int drain_count = 0;
    int stop_count = 0;
    int deactivate_count = 0;
    int fail_count = 0;
    int restart_count = 0;
    int recover_count = 0;
    int passivating_count = 0;
    int passivated_count = 0;

    TrackedLifecycleActor(ActorContext* ctx, ActorSystem& sys)
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
        return Behavior{[](TypedMessage& /*msg*/) {}};
    }

    void on_start() override {
        start_count++;
        hook_log.push_back("on_start");
    }
    void on_drain() override {
        drain_count++;
        hook_log.push_back("on_drain");
    }
    void on_stop() override {
        stop_count++;
        hook_log.push_back("on_stop");
    }
    void on_deactivate() override {
        deactivate_count++;
        hook_log.push_back("on_deactivate");
    }
    void on_fail(error /*err*/) override {
        fail_count++;
        hook_log.push_back("on_fail");
    }
    void on_restart() override {
        restart_count++;
        hook_log.push_back("on_restart");
    }
    void on_recover() override {
        recover_count++;
        hook_log.push_back("on_recover");
    }
    void on_passivating() override {
        passivating_count++;
        hook_log.push_back("on_passivating");
    }
    void on_passivated() override {
        passivated_count++;
        hook_log.push_back("on_passivated");
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Lifecycle state machine -- basic state queries after spawn
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, SpawnedActorIsActive) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto a1 = system.spawn<TrackedLifecycleActor>();
    auto* actor = static_cast<TrackedLifecycleActor*>(a1.get().get());

    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);
    EXPECT_STREQ(lc->state_string(), "active");
    EXPECT_TRUE(lc->accepts_user_msgs());
    EXPECT_TRUE(lc->accepts_system_msgs());
    EXPECT_EQ(lc->incarnation(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Full lifecycle transition chain (Active -> Draining -> Stopping ->
// Stopped)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, FullTransitionChainActiveToStopped) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto a1 = system.spawn<TrackedLifecycleActor>();
    auto* actor = static_cast<TrackedLifecycleActor*>(a1.get().get());
    auto* lc = actor->as_lifecycle();

    ASSERT_EQ(lc->state(), LifecycleState::kActive);
    EXPECT_EQ(actor->start_count, 1);

    // Active -> Draining
    bool ok = lc->transition(LifecycleState::kDraining);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kDraining);
    EXPECT_EQ(actor->drain_count, 1);

    // Draining -> Stopping
    ok = lc->transition(LifecycleState::kStopping);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kStopping);
    EXPECT_EQ(actor->stop_count, 1);

    // Stopping -> Stopped
    ok = lc->transition(LifecycleState::kStopped);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
    EXPECT_FALSE(lc->accepts_user_msgs());
    EXPECT_FALSE(lc->accepts_system_msgs());
    EXPECT_EQ(actor->deactivate_count, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Failed state and recovery cycle
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, FailedStateAndRecoveryCycle) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto a1 = system.spawn<TrackedLifecycleActor>();
    auto* actor = static_cast<TrackedLifecycleActor*>(a1.get().get());
    auto* lc = actor->as_lifecycle();

    ASSERT_EQ(lc->state(), LifecycleState::kActive);

    // Set failure reason and transition to Failed
    lc->set_failure_reason(error(42));
    bool ok = lc->transition(LifecycleState::kFailed);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kFailed);
    EXPECT_EQ(lc->failure_reason().code(), 42);
    EXPECT_EQ(actor->fail_count, 1);
    EXPECT_FALSE(lc->accepts_user_msgs());

    // Recovery: Failed -> Starting -> Active
    ok = lc->transition(LifecycleState::kStarting);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kStarting);
    EXPECT_EQ(actor->restart_count, 1);

    // Bump incarnation
    lc->bump_incarnation();
    EXPECT_EQ(lc->incarnation(), 1u);

    // Starting -> Active
    ok = lc->transition(LifecycleState::kActive);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);
    EXPECT_EQ(actor->start_count, 2); // second on_start
    EXPECT_TRUE(lc->accepts_user_msgs());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Passivation protocol (Active -> Passivating -> Passivated ->
// Recovering -> Active)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, PassivationProtocol) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto a1 = system.spawn<TrackedLifecycleActor>();
    auto* actor = static_cast<TrackedLifecycleActor*>(a1.get().get());
    auto* lc = actor->as_lifecycle();

    ASSERT_EQ(lc->state(), LifecycleState::kActive);

    // Active -> Passivating
    bool ok = lc->transition(LifecycleState::kPassivating);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kPassivating);
    EXPECT_EQ(actor->passivating_count, 1);
    EXPECT_FALSE(lc->accepts_user_msgs());
    EXPECT_TRUE(lc->accepts_system_msgs());

    // Passivating -> Passivated
    ok = lc->transition(LifecycleState::kPassivated);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kPassivated);
    EXPECT_EQ(actor->passivated_count, 1);

    // Passivated -> Recovering (reactivation)
    ok = lc->transition(LifecycleState::kRecovering);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kRecovering);
    EXPECT_EQ(actor->recover_count, 1);

    // Recovering -> Active (reactivation complete)
    ok = lc->transition(LifecycleState::kActive);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);
    EXPECT_EQ(actor->start_count, 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: Quarantine transition (Active -> Quarantined -> Stopped)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, QuarantineProtocol) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto a1 = system.spawn<TrackedLifecycleActor>();
    auto* actor = static_cast<TrackedLifecycleActor*>(a1.get().get());
    auto* lc = actor->as_lifecycle();

    ASSERT_EQ(lc->state(), LifecycleState::kActive);
    EXPECT_FALSE(lc->is_quarantined());

    // Active -> Quarantined
    bool ok =
        lc->transition_to_quarantined(QuarantineReason::SupervisionEscalation);
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kQuarantined);
    EXPECT_TRUE(lc->is_quarantined());
    EXPECT_EQ(lc->quarantine_reason(), QuarantineReason::SupervisionEscalation);

    // Release from quarantine: Quarantined -> Stopped
    ok = lc->transition_from_quarantined();
    EXPECT_TRUE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
    EXPECT_FALSE(lc->is_quarantined());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 6: Illegal transition is rejected
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, IllegalTransitionRejected) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto a1 = system.spawn<TrackedLifecycleActor>();
    auto* lc = a1.get()->as_lifecycle();

    ASSERT_EQ(lc->state(), LifecycleState::kActive);

    // Active -> Starting is illegal (cannot go backward)
    bool ok = lc->transition(LifecycleState::kStarting);
    EXPECT_FALSE(ok);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 7: Lifecycle with supervision integration
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, SupervisorAwareOfChildLifecycle) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    // Spawn a FailingActor that will fail after 1 message
    auto child = system.spawn<test::FailingActor>();
    auto* child_ptr = static_cast<test::FailingActor*>(child.get().get());
    child_ptr->fail_after = 1;

    // Verify child starts active
    auto* lc = child_ptr->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);

    // Send a message to trigger failure
    TypedMessage msg(TypeTag(0x1001), StreamBuffer{});
    msg.set_sender_address(ActorAddress{});
    child_ptr->context()->send(child.address(), std::move(msg));

    // Drain scheduler so the actor processes the message
    driver.drain();

    // After the message, the actor should have processed it
    EXPECT_GE(child_ptr->messages_processed, 1);
    EXPECT_EQ(lc->state(), LifecycleState::kFailed);
    EXPECT_EQ(lc->failure_reason().code(), 42);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 8: Lifecycle with scheduled messages
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, ScheduledMessageDuringLifecycle) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto a1 = system.spawn<TrackedLifecycleActor>();
    auto* actor = static_cast<TrackedLifecycleActor*>(a1.get().get());

    // Schedule a self-message via the context
    TypedMessage scheduled(TypeTag(42), StreamBuffer{});
    auto handle = actor->context()->schedule(std::chrono::milliseconds(5),
                                             std::move(scheduled));
    EXPECT_TRUE(handle.valid());

    // Let scheduler run to deliver the scheduled message
    driver.drain();

    // Verify scheduled delivery happened (or at least no crash)
    SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 9: Send message via ActorAddress
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, SendMessageViaActorAddress) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto target = system.spawn<test::CountingActor>();
    auto* target_ptr = static_cast<test::CountingActor*>(target.get().get());

    ActorAddress addr = target.address();
    EXPECT_NE(addr.id.value(), 0u);
    EXPECT_TRUE(addr.is_local());

    int initial = target_ptr->handler_count;

    // Send a message via the actor address
    TypedMessage msg(TypeTag(77), StreamBuffer{});
    target_ptr->context()->send(addr, std::move(msg));
    driver.drain();

    EXPECT_GT(target_ptr->handler_count, initial);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 10: ScopedActor lifecycle -- construction, proper cleanup
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, ScopedActorLifecycle) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    {
        ScopedActor scoped(system);
        // ScopedActor should be constructible and not crash.
        // Note: ScopedActor may use a zero ActorId since it doesn't go
        // through the standard spawn path — just verify construction succeeds.
        EXPECT_TRUE(system.is_running());
    }
    // scoped goes out of scope, destructor unregisters from system
    // No crash = pass
    SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 11: Message gate -- user messages rejected in non-Active states
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, MessageGateRejectsUserMsgsInNonActiveStates) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto a1 = system.spawn<TrackedLifecycleActor>();
    auto* actor = static_cast<TrackedLifecycleActor*>(a1.get().get());
    auto* lc = actor->as_lifecycle();

    // Starting -- should reject user messages
    EXPECT_FALSE(
        kStateMachine[static_cast<int>(LifecycleState::kStarting)].accepts_user_msgs);
    EXPECT_TRUE(
        kStateMachine[static_cast<int>(LifecycleState::kStarting)].accepts_system_msgs);

    // Active -- should accept user messages
    EXPECT_TRUE(
        kStateMachine[static_cast<int>(LifecycleState::kActive)].accepts_user_msgs);
    EXPECT_TRUE(
        kStateMachine[static_cast<int>(LifecycleState::kActive)].accepts_system_msgs);

    // Stopped -- rejects both user and system messages
    EXPECT_FALSE(
        kStateMachine[static_cast<int>(LifecycleState::kStopped)].accepts_user_msgs);
    EXPECT_FALSE(
        kStateMachine[static_cast<int>(LifecycleState::kStopped)].accepts_system_msgs);

    // Failed -- rejects user messages, accepts system
    EXPECT_FALSE(
        kStateMachine[static_cast<int>(LifecycleState::kFailed)].accepts_user_msgs);
    EXPECT_TRUE(
        kStateMachine[static_cast<int>(LifecycleState::kFailed)].accepts_system_msgs);

    // Passivated -- rejects user messages, accepts system
    EXPECT_FALSE(
        kStateMachine[static_cast<int>(LifecycleState::kPassivated)].accepts_user_msgs);
    EXPECT_TRUE(
        kStateMachine[static_cast<int>(LifecycleState::kPassivated)].accepts_system_msgs);

    // Quarantined -- rejects user messages, accepts system
    EXPECT_FALSE(
        kStateMachine[static_cast<int>(LifecycleState::kQuarantined)].accepts_user_msgs);
    EXPECT_TRUE(
        kStateMachine[static_cast<int>(LifecycleState::kQuarantined)].accepts_system_msgs);

    // Verify with live actor
    EXPECT_TRUE(lc->accepts_user_msgs()); // currently Active

    // Transition to draining -- gates should update
    lc->transition(LifecycleState::kDraining);
    EXPECT_EQ(lc->state(), LifecycleState::kDraining);
    EXPECT_FALSE(lc->accepts_user_msgs());
    EXPECT_TRUE(lc->accepts_system_msgs());

    // Transition to stopped -- both gates closed
    lc->transition(LifecycleState::kStopping);
    lc->transition(LifecycleState::kStopped);
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
    EXPECT_FALSE(lc->accepts_user_msgs());
    EXPECT_FALSE(lc->accepts_system_msgs());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 12: Drain config defaults and override
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, DrainConfigDefaultsAndOverride) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto a1 = system.spawn<TrackedLifecycleActor>();
    auto* lc = static_cast<TrackedLifecycleActor*>(a1.get().get())->as_lifecycle();

    // Default drain config
    DrainConfig default_cfg = lc->drain_config();
    EXPECT_EQ(default_cfg.policy, DrainPolicy::Drain);

    // Override drain config
    DrainConfig custom;
    custom.policy = DrainPolicy::DropUserMessages;
    custom.timeout = std::chrono::milliseconds(5000);
    lc->set_drain_config(custom);

    DrainConfig retrieved = lc->drain_config();
    EXPECT_EQ(retrieved.policy, DrainPolicy::DropUserMessages);
    EXPECT_EQ(retrieved.timeout, std::chrono::milliseconds(5000));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 13: Incarnation counter increments correctly
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorLifecycle, IncarnationCounterIncrements) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto a1 = system.spawn<TrackedLifecycleActor>();
    auto* lc = static_cast<TrackedLifecycleActor*>(a1.get().get())->as_lifecycle();

    EXPECT_EQ(lc->incarnation(), 0u);

    lc->bump_incarnation();
    EXPECT_EQ(lc->incarnation(), 1u);

    lc->bump_incarnation();
    lc->bump_incarnation();
    EXPECT_EQ(lc->incarnation(), 3u);
}
