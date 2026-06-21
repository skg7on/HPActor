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

// System test: Actor subsystem deep coverage
// Targets src/actor and src/actor/lifecycle code paths including spawn,
// context send/reply/schedule, lifecycle transitions, and message routing.

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/msg/type_tag.hpp>

#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hpactor;

// ── Register test actors for spawn ───────────────────────────────────────────

using CountingActor = test::CountingActor;
using EchoActor = test::EchoActor;
using ForwardingActor = test::ForwardingActor;
using FailingActor = test::FailingActor;

HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);
HPACTOR_REGISTER_ACTOR("EchoActor", EchoActor);
HPACTOR_REGISTER_ACTOR("ForwardingActor", ForwardingActor);
HPACTOR_REGISTER_ACTOR("FailingActor", FailingActor);

// ── ReplyActor: echoes via context()->reply() instead of just counting ───────

class ReplyActor : public EventBasedActor, public LifecycleActor {
  public:
    int messages_replied = 0;
    std::vector<TypeTag> received_tags;
    ActorAddress last_reply_target;

    ReplyActor(ActorContext* ctx, ActorSystem& sys)
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
            messages_replied++;
            received_tags.push_back(msg.type_id());
            last_reply_target = msg.sender_address();
            // Echo back as reply so sender sees the round-trip
            context()->reply(std::move(msg));
        }};
    }
};

HPACTOR_REGISTER_ACTOR("ReplyActor", ReplyActor);

// ── ErrorReplyActor: always replies with an error ────────────────────────────

class ErrorReplyActor : public EventBasedActor, public LifecycleActor {
  public:
    int messages_received = 0;
    int errors_sent = 0;

    ErrorReplyActor(ActorContext* ctx, ActorSystem& sys)
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
        return Behavior{[this](TypedMessage& /*msg*/) {
            messages_received++;
            context()->reply_with_error(error(42, "test error"));
            errors_sent++;
        }};
    }
};

HPACTOR_REGISTER_ACTOR("ErrorReplyActor", ErrorReplyActor);

// ═══════════════════════════════════════════════════════════════════════════════
// Group 1: ActorSystem deep
// ═══════════════════════════════════════════════════════════════════════════════

// Test 1.1: Spawn with constructor args exercises the variadic
// spawn<T>(Args...)
TEST(ActorDeepWorkflow, ActorSystemSpawnWithArgs) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    // ForwardingActor takes an optional ActorAddress arg
    ActorAddress dummy_target;
    dummy_target.id = ActorId(42);

    auto a1 = system.spawn<test::CountingActor>();
    auto a2 = system.spawn<test::ForwardingActor>(dummy_target);

    EXPECT_NE(a1.get(), nullptr);
    EXPECT_NE(a2.get(), nullptr);

    auto* fwd = static_cast<test::ForwardingActor*>(a2.get().get());
    EXPECT_EQ(fwd->target.id, dummy_target.id);

    // spawn<CountingActor> should have kActive lifecycle
    auto* lc = static_cast<test::CountingActor*>(a1.get().get())->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);
}

// Test 1.2: actor_count reflects spawns; resolve_actor by registered name
TEST(ActorDeepWorkflow, ActorSystemActorCountAndResolve) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    size_t initial = system.actor_count();

    auto a1 = system.spawn<test::CountingActor>();
    auto a2 = system.spawn<test::EchoActor>();

    EXPECT_GE(system.actor_count(), initial + 2);

    // register a name and resolve it
    system.register_actor("county", a1);
    auto resolved = system.resolve_actor("county");
    EXPECT_EQ(resolved.address(), a1.address());

    // resolve unknown name returns empty Actor
    auto unknown = system.resolve_actor("nonexistent");
    EXPECT_EQ(unknown.get(), nullptr);

    // unregister removes from the registry; resolve_actor may still
    // find the actor through the directory
    system.unregister_actor("county");
    auto still_there = system.resolve_actor("county");
    EXPECT_NE(still_there.get(), nullptr);
    EXPECT_EQ(still_there.address(), a1.address());

    // register with a new name works after unregister
    system.register_actor("county_v2", a1);
    auto renamed = system.resolve_actor("county_v2");
    EXPECT_NE(renamed.get(), nullptr);
    EXPECT_EQ(renamed.address(), a1.address());
}

// Test 1.3: system_actor() and is_system_actor() detection
TEST(ActorDeepWorkflow, ActorSystemSystemActorDetection) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    // system_actor() returns the system pseudo-actor (may be null
    // when no subsystems such as metrics/logging are active)
    auto sys_actor = system.system_actor();
    if (sys_actor.get() != nullptr) {
        EXPECT_TRUE(sys_actor.get()->is_system_actor());
    }

    // User-spawned actors are NOT system actors
    auto user = system.spawn<test::CountingActor>();
    EXPECT_FALSE(user.get()->is_system_actor());
    EXPECT_FALSE(
        static_cast<test::CountingActor*>(user.get().get())->is_system_actor());

    // get_actor by id
    auto found = system.get_actor(user.id());
    EXPECT_NE(found, nullptr);
    EXPECT_EQ(found->address(), user.address());

    // get_actor for unknown id returns nullptr
    auto not_found = system.get_actor(ActorId(99999));
    EXPECT_EQ(not_found, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Group 2: ActorContext deep
// ═══════════════════════════════════════════════════════════════════════════════

// Test 2.1: Context send with delivery mode (priority + try_send receipt)
TEST(ActorDeepWorkflow, ContextSendWithDeliveryMode) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto sender = system.spawn<test::CountingActor>();
    auto receiver = system.spawn<test::CountingActor>();
    auto* recv_ptr = static_cast<test::CountingActor*>(receiver.get().get());

    test::SchedulerTestDriver driver(system);

    // deliver_local with priority exercises the priority delivery path
    TypedMessage msg(TypeTag(0x2001), StreamBuffer{});
    msg.set_sender_address(sender.address());
    system.deliver_local(receiver.id(), std::move(msg), /*priority=*/5,
                         /*deadline_ns=*/INT64_MAX);

    driver.drain_until([&]() { return recv_ptr->handler_count >= 1; });
    EXPECT_GE(recv_ptr->handler_count, 1);
    EXPECT_EQ(recv_ptr->received_type_ids[0], 0x2001);

    // try_deliver_local returns an EnqueueResult
    TypedMessage msg2(TypeTag(0x2002), StreamBuffer{});
    msg2.set_sender_address(sender.address());
    auto result = system.try_deliver_local(receiver.id(), std::move(msg2));
    EXPECT_TRUE(result.accepted());

    driver.drain_until([&]() { return recv_ptr->handler_count >= 2; });
    EXPECT_GE(recv_ptr->handler_count, 2);
}

// Test 2.2: Context schedule with AlarmHandle — message arrives after delay.
// Uses unpaused scheduler so timers fire; assert_eventually polls for delivery.
TEST(ActorDeepWorkflow, ContextScheduleWithAlarmHandle) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto actor = system.spawn<test::CountingActor>();
    auto* ptr = static_cast<test::CountingActor*>(actor.get().get());

    // Schedule a self-message via context->schedule()
    TypedMessage scheduled_msg(TypeTag(0x3001), StreamBuffer{});
    auto handle = ptr->context()->schedule(std::chrono::milliseconds(100),
                                           std::move(scheduled_msg));
    EXPECT_NE(handle.value(), 0);

    // Cancel the first schedule — should never be delivered
    ptr->context()->cancel_schedule(handle);

    // Schedule another with a short delay that we will let fire
    TypedMessage scheduled_msg2(TypeTag(0x3002), StreamBuffer{});
    auto handle2 = ptr->context()->schedule(std::chrono::milliseconds(10),
                                            std::move(scheduled_msg2));
    EXPECT_NE(handle2.value(), 0);

    // Wait for the active timer to fire
    bool delivered =
        test::assert_eventually([&]() { return ptr->handler_count >= 1; }, 5000);
    EXPECT_TRUE(delivered);
    EXPECT_GE(ptr->handler_count, 1);

    // The first cancelled message should not have fired
    bool found_3001 = false;
    for (auto tag : ptr->received_type_ids) {
        if (tag == 0x3001)
            found_3001 = true;
    }
    EXPECT_FALSE(found_3001);

    // cancel with zero handle is a no-op
    ptr->context()->cancel_schedule(AlarmHandle{0});
}

// Test 2.3: Context reply and reply_with_error.
// Uses unpaused scheduler so reply delivery completes normally.
TEST(ActorDeepWorkflow, ContextReplyAndReplyWithError) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto reply_actor = system.spawn<ReplyActor>();
    auto error_actor = system.spawn<ErrorReplyActor>();
    auto observer = system.spawn<test::CountingActor>();

    auto* reply_ptr = static_cast<ReplyActor*>(reply_actor.get().get());
    auto* error_ptr = static_cast<ErrorReplyActor*>(error_actor.get().get());
    auto* obs_ptr = static_cast<test::CountingActor*>(observer.get().get());

    // Send a message from observer to reply_actor — reply_actor calls reply()
    TypedMessage msg(TypeTag(0x4001), StreamBuffer{});
    msg.set_sender_address(observer.address());
    system.deliver_local(reply_actor.id(), std::move(msg));

    bool replied = test::assert_eventually(
        [&]() { return reply_ptr->messages_replied >= 1; }, 5000);
    EXPECT_TRUE(replied);
    EXPECT_EQ(reply_ptr->messages_replied, 1);
    EXPECT_EQ(reply_ptr->last_reply_target.id, observer.address().id);

    // reply() should deliver back to observer
    bool obs_got_reply = test::assert_eventually(
        [&]() { return obs_ptr->handler_count >= 1; }, 5000);
    EXPECT_TRUE(obs_got_reply);
    EXPECT_GE(obs_ptr->handler_count, 1);

    // Send a message to error_actor — it calls reply_with_error()
    TypedMessage msg2(TypeTag(0x4002), StreamBuffer{});
    msg2.set_sender_address(observer.address());
    system.deliver_local(error_actor.id(), std::move(msg2));

    bool err_replied = test::assert_eventually(
        [&]() { return error_ptr->errors_sent >= 1; }, 5000);
    EXPECT_TRUE(err_replied);
    EXPECT_GE(error_ptr->errors_sent, 1);

    // The error message (TypeTag::ErrorMsg) should be received by observer
    bool obs_got_error = test::assert_eventually(
        [&]() {
            for (auto tag : obs_ptr->received_type_ids) {
                if (tag == static_cast<uint32_t>(TypeTag::ErrorMsg))
                    return true;
            }
            return false;
        },
        5000);
    EXPECT_TRUE(obs_got_error);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Group 3: Spawn + lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

// Test 3.1: Spawn different actor types and verify each is active
TEST(ActorDeepWorkflow, SpawnDifferentActorTypes) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto counting = system.spawn<test::CountingActor>();
    auto echo = system.spawn<test::EchoActor>();
    auto fwd = system.spawn<test::ForwardingActor>(ActorAddress{});
    auto reply = system.spawn<ReplyActor>();
    auto err_reply = system.spawn<ErrorReplyActor>();

    EXPECT_NE(counting.get(), nullptr);
    EXPECT_NE(echo.get(), nullptr);
    EXPECT_NE(fwd.get(), nullptr);
    EXPECT_NE(reply.get(), nullptr);
    EXPECT_NE(err_reply.get(), nullptr);

    // All should be in kActive after spawn
    for (auto* actor_ptr : {static_cast<AbstractActor*>(counting.get().get()),
                            static_cast<AbstractActor*>(echo.get().get()),
                            static_cast<AbstractActor*>(fwd.get().get()),
                            static_cast<AbstractActor*>(reply.get().get()),
                            static_cast<AbstractActor*>(err_reply.get().get())}) {
        auto* lc = actor_ptr->as_lifecycle();
        ASSERT_NE(lc, nullptr);
        EXPECT_EQ(lc->state(), LifecycleState::kActive);
        EXPECT_TRUE(lc->accepts_user_msgs());
        EXPECT_TRUE(lc->accepts_system_msgs());
        EXPECT_STREQ(lc->state_string(), "active");
    }

    EXPECT_GE(system.actor_count(), 5);
}

// Test 3.2: Lifecycle state queries after spawn — incarnation and state API
TEST(ActorDeepWorkflow, LifecycleStateQueriesAfterSpawn) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* lc = static_cast<test::CountingActor*>(a1.get().get())->as_lifecycle();

    // Right after spawn, should be kActive
    EXPECT_EQ(lc->state(), LifecycleState::kActive);
    EXPECT_TRUE(lc->accepts_user_msgs());
    EXPECT_TRUE(lc->accepts_system_msgs());

    // Incarnation starts at 0, can be bumped
    EXPECT_EQ(lc->incarnation(), 0);
    lc->bump_incarnation();
    EXPECT_EQ(lc->incarnation(), 1);

    // Drain config has defaults
    auto dc = lc->drain_config();
    EXPECT_EQ(dc.policy, DrainPolicy::Drain);
}

// Test 3.3: Lifecycle full transition cycle (active -> draining -> stopping ->
// stopped)
TEST(ActorDeepWorkflow, LifecycleFullTransitionCycle) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* lc = static_cast<test::CountingActor*>(a1.get().get())->as_lifecycle();

    // Active -> Draining -> Stopping -> Stopped
    EXPECT_EQ(lc->state(), LifecycleState::kActive);

    EXPECT_TRUE(lc->transition(LifecycleState::kDraining));
    EXPECT_EQ(lc->state(), LifecycleState::kDraining);
    EXPECT_FALSE(lc->accepts_user_msgs());
    EXPECT_TRUE(lc->accepts_system_msgs());

    EXPECT_TRUE(lc->transition(LifecycleState::kStopping));
    EXPECT_EQ(lc->state(), LifecycleState::kStopping);

    EXPECT_TRUE(lc->transition(LifecycleState::kStopped));
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
    EXPECT_FALSE(lc->accepts_user_msgs());
    EXPECT_FALSE(lc->accepts_system_msgs());

    // Stopped -> Starting (restart) -> Active
    EXPECT_TRUE(lc->transition(LifecycleState::kStarting));
    EXPECT_EQ(lc->state(), LifecycleState::kStarting);

    EXPECT_TRUE(lc->transition(LifecycleState::kActive));
    EXPECT_EQ(lc->state(), LifecycleState::kActive);

    // Illegal transitions should fail
    EXPECT_FALSE(lc->transition(LifecycleState::kStarting)); // active ->
                                                             // starting illegal
    EXPECT_EQ(lc->state(), LifecycleState::kActive);         // state unchanged
    EXPECT_FALSE(lc->transition(LifecycleState::kRecovering)); // active ->
                                                               // recovering
                                                               // illegal
}

// Test 3.4: Lifecycle failure and quarantine paths
TEST(ActorDeepWorkflow, LifecycleFailureAndQuarantinePaths) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* lc = static_cast<test::CountingActor*>(a1.get().get())->as_lifecycle();

    // Active -> Failed
    lc->set_failure_reason(error(99, "test failure"));
    EXPECT_TRUE(lc->transition(LifecycleState::kFailed));
    EXPECT_EQ(lc->state(), LifecycleState::kFailed);
    EXPECT_EQ(lc->failure_reason().code(), 99);
    EXPECT_FALSE(lc->accepts_user_msgs());

    // Failed -> Recovering
    EXPECT_TRUE(lc->transition(LifecycleState::kRecovering));
    EXPECT_EQ(lc->state(), LifecycleState::kRecovering);

    // Recovering -> Active
    EXPECT_TRUE(lc->transition(LifecycleState::kActive));
    EXPECT_EQ(lc->state(), LifecycleState::kActive);

    // Active -> Quarantined
    EXPECT_FALSE(lc->is_quarantined());
    EXPECT_TRUE(lc->transition_to_quarantined(QuarantineReason::CircuitBreakerTrip));
    EXPECT_EQ(lc->state(), LifecycleState::kQuarantined);
    EXPECT_TRUE(lc->is_quarantined());
    EXPECT_EQ(lc->quarantine_reason(), QuarantineReason::CircuitBreakerTrip);
    EXPECT_FALSE(lc->accepts_user_msgs());

    // Quarantined -> Stopped (release from quarantine)
    EXPECT_TRUE(lc->transition_from_quarantined());
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
    EXPECT_FALSE(lc->is_quarantined());

    // transition_from_quarantined when not quarantined is a no-op
    EXPECT_FALSE(lc->transition_from_quarantined());
}

// Test 3.5: Lifecycle passivation path
TEST(ActorDeepWorkflow, LifecyclePassivationPath) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* lc = static_cast<test::CountingActor*>(a1.get().get())->as_lifecycle();

    EXPECT_EQ(lc->state(), LifecycleState::kActive);

    // Active -> Passivating
    EXPECT_TRUE(lc->transition(LifecycleState::kPassivating));
    EXPECT_EQ(lc->state(), LifecycleState::kPassivating);
    EXPECT_FALSE(lc->accepts_user_msgs());
    EXPECT_TRUE(lc->accepts_system_msgs());

    // Passivating -> Passivated
    EXPECT_TRUE(lc->transition(LifecycleState::kPassivated));
    EXPECT_EQ(lc->state(), LifecycleState::kPassivated);
    EXPECT_FALSE(lc->accepts_user_msgs());
    EXPECT_TRUE(lc->accepts_system_msgs());

    // Passivated -> Recovering
    EXPECT_TRUE(lc->transition(LifecycleState::kRecovering));
    EXPECT_EQ(lc->state(), LifecycleState::kRecovering);

    // Recovering -> Active
    EXPECT_TRUE(lc->transition(LifecycleState::kActive));
    EXPECT_EQ(lc->state(), LifecycleState::kActive);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Group 4: Message routing
// ═══════════════════════════════════════════════════════════════════════════════

// Test 4.1: TypedMessage dispatch with various type tags
TEST(ActorDeepWorkflow, TypedMessageDispatchWithTypeTags) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* ptr = static_cast<test::CountingActor*>(a1.get().get());

    test::SchedulerTestDriver driver(system);

    // Deliver messages with distinct type tags
    std::vector<uint32_t> tags = {0xA001, 0xA002, 0xA003, 0xA004, 0xA005};
    for (auto tag : tags) {
        TypedMessage msg(TypeTag(tag), StreamBuffer{});
        msg.set_sender_address(ActorAddress{});
        system.deliver_local(a1.id(), std::move(msg));
    }

    driver.drain_until([&]() { return ptr->handler_count >= 5; });
    EXPECT_EQ(ptr->handler_count, 5);

    // Verify all type tags were received
    EXPECT_EQ(ptr->received_type_ids.size(), 5u);
    for (size_t i = 0; i < tags.size(); ++i) {
        EXPECT_EQ(ptr->received_type_ids[i], tags[i]);
    }
}

// Test 4.2: System message routing - link/unlink via system message dispatch
TEST(ActorDeepWorkflow, SystemMessageRoutingLinkUnlinkDown) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto a2 = system.spawn<test::CountingActor>();
    auto* actor1 = static_cast<test::CountingActor*>(a1.get().get());

    test::SchedulerTestDriver driver(system);

    // Dispatch a LinkMsg system message: adds a2 to a1's linked list
    TypedMessage link_msg(TypeTag::LinkMsg, StreamBuffer{});
    link_msg.set_sender_address(a2.address());
    system.deliver_local(a1.id(), std::move(link_msg));

    driver.drain();
    auto linked = actor1->context()->linked_actors();
    EXPECT_FALSE(linked.empty());
    EXPECT_EQ(linked[0], a2.address());

    // Dispatch an UnlinkMsg: removes a2 from a1's linked list
    TypedMessage unlink_msg(TypeTag::UnlinkMsg, StreamBuffer{});
    unlink_msg.set_sender_address(a2.address());
    system.deliver_local(a1.id(), std::move(unlink_msg));

    driver.drain();
    linked = actor1->context()->linked_actors();
    EXPECT_TRUE(linked.empty());

    // Link again, then dispatch DownMsg: removes from both linked and monitored
    actor1->link_to(a2.address());
    driver.drain();

    TypedMessage down_msg(TypeTag::DownMsg, StreamBuffer{});
    down_msg.set_sender_address(a2.address());
    system.deliver_local(a1.id(), std::move(down_msg));

    driver.drain();
    linked = actor1->context()->linked_actors();
    EXPECT_TRUE(linked.empty());
}

// Test 4.3: Monitor and demonitor via system messages
TEST(ActorDeepWorkflow, MonitorDemonitorSystemMessages) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto a2 = system.spawn<test::CountingActor>();

    test::SchedulerTestDriver driver(system);

    // Send MonitorMsg from a2 to a1
    TypedMessage mon_msg(TypeTag::MonitorMsg, StreamBuffer{});
    mon_msg.set_sender_address(a2.address());
    system.deliver_local(a1.id(), std::move(mon_msg));

    driver.drain();

    // Send DemonitorMsg
    TypedMessage demon_msg(TypeTag::DemonitorMsg, StreamBuffer{});
    demon_msg.set_sender_address(a2.address());
    system.deliver_local(a1.id(), std::move(demon_msg));

    driver.drain();
    // No crash = success; the system message dispatch path exercised
}

// Test 4.4: Message delivery to terminated actor — observe that stopped actors
//           do not process user messages (lifecycle gate rejects)
TEST(ActorDeepWorkflow, MessageDeliveryToTerminatedActor) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* ptr = static_cast<test::CountingActor*>(a1.get().get());
    auto* lc = ptr->as_lifecycle();

    test::SchedulerTestDriver driver(system);

    // First, deliver a message that should be processed normally
    TypedMessage msg1(TypeTag(0xB001), StreamBuffer{});
    msg1.set_sender_address(ActorAddress{});
    system.deliver_local(a1.id(), std::move(msg1));

    driver.drain_until([&]() { return ptr->handler_count >= 1; });
    EXPECT_GE(ptr->handler_count, 1);

    // Now stop the actor
    lc->transition(LifecycleState::kStopping);
    lc->transition(LifecycleState::kStopped);
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);

    int handler_before = ptr->handler_count;

    // Deliver to stopped actor — user messages should be rejected by lifecycle
    // gate
    TypedMessage msg2(TypeTag(0xB002), StreamBuffer{});
    msg2.set_sender_address(ActorAddress{});
    system.deliver_local(a1.id(), std::move(msg2));

    driver.drain();

    // Handler count should not increase; user messages are rejected at kStopped
    EXPECT_EQ(ptr->handler_count, handler_before);
}

// Test 4.5: for_each_actor enumerates correctly with mixed actor types
TEST(ActorDeepWorkflow, ForEachActorEnumeratesAllTypes) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    system.spawn<test::CountingActor>();
    system.spawn<test::EchoActor>();
    system.spawn<test::ForwardingActor>(ActorAddress{});
    system.spawn<ReplyActor>();

    int user_count = 0;
    int system_count = 0;
    system.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        if (actor.is_system_actor()) {
            system_count++;
        } else {
            user_count++;
        }
    });

    EXPECT_GE(user_count, 4);
    // System actors may be present (metrics, etc.) depending on build config
    EXPECT_GE(system_count, 0);
}

// Test 4.6: deliver_local with deadline exercises the EDF delivery path
TEST(ActorDeepWorkflow, DeliverLocalWithPriorityAndDeadline) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto a1 = system.spawn<test::CountingActor>();
    auto* ptr = static_cast<test::CountingActor*>(a1.get().get());

    test::SchedulerTestDriver driver(system);

    // Send two messages with different priorities; higher-priority (lower
    // value) should be processed first when using priority scheduling
    TypedMessage high_prio(TypeTag(0xC001), StreamBuffer{});
    high_prio.set_sender_address(ActorAddress{});
    system.deliver_local(a1.id(), std::move(high_prio), /*priority=*/0,
                         /*deadline_ns=*/INT64_MAX);

    TypedMessage low_prio(TypeTag(0xC002), StreamBuffer{});
    low_prio.set_sender_address(ActorAddress{});
    system.deliver_local(a1.id(), std::move(low_prio), /*priority=*/255,
                         /*deadline_ns=*/INT64_MAX);

    driver.drain_until([&]() { return ptr->handler_count >= 2; });
    EXPECT_GE(ptr->handler_count, 2);
}
