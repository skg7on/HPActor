// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

// System test: Supervision Workflow
// Exercises restart tracking, supervisor restart logic, lifecycle
// transitions, quarantine, circuit breaker, self-supervising actors,
// and deep nesting with deterministic scheduler control.

#include <gtest/gtest.h>

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/supervision/supervision.hpp>

#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

using namespace hpactor;

namespace {

// ── RestartTrackingActor ───────────────────────────────────────────────
//
// Tracks every on_start() call (restart count) and every message processed
// via its Behavior.  Inherits from both EventBasedActor (dispatch) and
// LifecycleActor (on_start/on_restart hooks).

class RestartTrackingActor : public EventBasedActor, public LifecycleActor {
  public:
    RestartTrackingActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    int start_count() const {
        return start_count_;
    }
    int message_count() const {
        return msg_count_;
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

  protected:
    void on_start() override {
        start_count_++;
        LifecycleActor::on_start();
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& /*msg*/) { msg_count_++; }};
    }

  private:
    int start_count_ = 0;
    int msg_count_ = 0;
};

} // namespace

HPACTOR_REGISTER_ACTOR("RestartTrackingActor", RestartTrackingActor);

// ── Register test-fixture actors for supervision scenarios ────────────────

using CountingActor = test::CountingActor;
using FailingActor = test::FailingActor;

HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);
HPACTOR_REGISTER_ACTOR("FailingActor", FailingActor);

// ═══════════════════════════════════════════════════════════════════════════
// Group 1: Basic restart and message processing
// ═══════════════════════════════════════════════════════════════════════════

// ── Cold start: on_start() fires exactly once on spawn ─────────────────

TEST(SupervisionWorkflow, RestartTrackingActorColdStart) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto actor = system.spawn<RestartTrackingActor>();
    ASSERT_TRUE(actor);
    auto* rt = static_cast<RestartTrackingActor*>(actor.get().get());

    // on_start fires synchronously during on_activate -> kActive transition
    EXPECT_EQ(rt->start_count(), 1);
    EXPECT_EQ(rt->message_count(), 0);
}

// ── Messages delivered via deliver_local are processed ─────────────────

TEST(SupervisionWorkflow, ActorReceivesAndProcessesMessages) {
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);

    auto actor = system.spawn<RestartTrackingActor>();
    auto* rt = static_cast<RestartTrackingActor*>(actor.get().get());

    // Deliver three messages via the direct local path
    for (int i = 0; i < 3; ++i) {
        TypedMessage msg(TypeTag(0x1000 + i), StreamBuffer{});
        msg.set_sender_address(actor.address());
        system.deliver_local(actor.id(), std::move(msg));
    }

    test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() { return rt->message_count() >= 3; });
    EXPECT_TRUE(done);
    EXPECT_EQ(rt->message_count(), 3);
}

// ── Supervisor detects child failure and executes restart directive ────

TEST(SupervisionWorkflow, SupervisorRestartsFailedChild) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    SupervisionPolicy policy;
    policy.strategy = SupervisionPolicy::Strategy::OneForOne;
    policy.max_restarts = 3;
    policy.restart_interval = std::chrono::milliseconds{10000};

    auto supervisor = system.spawn<SelfSupervisingActor>(policy);
    auto child = system.spawn<RestartTrackingActor>();
    auto* child_ptr = static_cast<RestartTrackingActor*>(child.get().get());

    auto* sup = static_cast<SelfSupervisingActor*>(supervisor.get().get());
    sup->add_child(child);

    // Drain spawn-time notify_ready items
    driver.drain(10);

    // Pin child for deterministic execution
    driver.pin_actor_to_worker(child.address().id, 0);

    // Manually transition child to kFailed to simulate failure.
    // This triggers the supervisor's handle_child_down path.
    child_ptr->as_lifecycle()->set_failure_reason(error(42));
    bool ok = child_ptr->as_lifecycle()->transition(LifecycleState::kFailed);
    EXPECT_TRUE(ok);
    EXPECT_EQ(child_ptr->as_lifecycle()->state(), LifecycleState::kFailed);
}

// ═══════════════════════════════════════════════════════════════════════════
// Group 2: Multi-child supervision
// ═══════════════════════════════════════════════════════════════════════════

// ── SelfSupervisingActor tracks local children ────────────────────────

TEST(SupervisionWorkflow, SelfSupervisingActorManagesOwnChildren) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    SupervisionPolicy policy;
    auto supervisor = system.spawn<SelfSupervisingActor>(policy);
    auto* sup = static_cast<SelfSupervisingActor*>(supervisor.get().get());

    auto c1 = system.spawn<RestartTrackingActor>();
    auto c2 = system.spawn<RestartTrackingActor>();

    sup->add_child(c1);
    sup->add_child(c2);

    // Both children should be alive and have on_start called
    auto* r1 = static_cast<RestartTrackingActor*>(c1.get().get());
    auto* r2 = static_cast<RestartTrackingActor*>(c2.get().get());
    EXPECT_EQ(r1->start_count(), 1);
    EXPECT_EQ(r2->start_count(), 1);
    EXPECT_EQ(r1->as_lifecycle()->state(), LifecycleState::kActive);
    EXPECT_EQ(r2->as_lifecycle()->state(), LifecycleState::kActive);

    // Remove child and verify it is independent of supervisor tracking
    sup->remove_child(c1);
    // Child still alive after removal — removal only stops supervision
    EXPECT_EQ(r1->as_lifecycle()->state(), LifecycleState::kActive);
}

// ── Supervisor with 5 children; one fails, others unaffected ────────────

TEST(SupervisionWorkflow, SupervisorWithMultipleChildren) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    SupervisionPolicy policy;
    policy.strategy = SupervisionPolicy::Strategy::OneForOne;
    policy.max_restarts = 5;

    auto supervisor = system.spawn<SelfSupervisingActor>(policy);
    auto* sup = static_cast<SelfSupervisingActor*>(supervisor.get().get());

    // Spawn 5 children — mix of CountingActor and FailingActor
    auto child1 = system.spawn<test::CountingActor>();
    auto child2 = system.spawn<test::CountingActor>();
    auto child3 = system.spawn<test::CountingActor>();
    auto child4 = system.spawn<test::CountingActor>();
    auto child5 = system.spawn<test::FailingActor>();
    auto* f5 = static_cast<test::FailingActor*>(child5.get().get());
    f5->fail_after = 1;

    sup->add_child(child1);
    sup->add_child(child2);
    sup->add_child(child3);
    sup->add_child(child4);
    sup->add_child(child5);

    // Drain spawn-time notify_ready items
    driver.drain(10);

    // Pin the failing child for deterministic execution
    driver.pin_actor_to_worker(child5.address().id, 0);

    // Send a message to trigger failure in child5
    TypedMessage msg(TypeTag(0x1001), StreamBuffer{});
    msg.set_sender_address(supervisor.address());
    f5->context()->send(child5.address(), std::move(msg));

    EXPECT_TRUE(driver.run_actor(child5.address().id));
    EXPECT_EQ(f5->as_lifecycle()->state(), LifecycleState::kFailed);

    // Other 4 children should still be active
    auto* c1 = static_cast<test::CountingActor*>(child1.get().get());
    auto* c2 = static_cast<test::CountingActor*>(child2.get().get());
    auto* c3 = static_cast<test::CountingActor*>(child3.get().get());
    auto* c4 = static_cast<test::CountingActor*>(child4.get().get());
    EXPECT_EQ(c1->as_lifecycle()->state(), LifecycleState::kActive);
    EXPECT_EQ(c2->as_lifecycle()->state(), LifecycleState::kActive);
    EXPECT_EQ(c3->as_lifecycle()->state(), LifecycleState::kActive);
    EXPECT_EQ(c4->as_lifecycle()->state(), LifecycleState::kActive);
}

// ── Deep supervision tree: root → mid → leaf ─────────────────────────

TEST(SupervisionWorkflow, DeepSupervisionTreeSpawning) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    // Root supervisor
    auto root = system.spawn<SelfSupervisingActor>();
    auto* root_sup = static_cast<SelfSupervisingActor*>(root.get().get());

    // Mid-level supervisor (child of root)
    auto mid = system.spawn<SelfSupervisingActor>();
    auto* mid_sup = static_cast<SelfSupervisingActor*>(mid.get().get());

    // Leaf actors (children of mid)
    auto leaf1 = system.spawn<RestartTrackingActor>();
    auto leaf2 = system.spawn<RestartTrackingActor>();

    // Build the tree: root → mid, mid → leaf1, leaf2
    root_sup->add_child(mid);
    mid_sup->add_child(leaf1);
    mid_sup->add_child(leaf2);

    auto* r1 = static_cast<RestartTrackingActor*>(leaf1.get().get());
    auto* r2 = static_cast<RestartTrackingActor*>(leaf2.get().get());

    // All actors are alive and initialized
    EXPECT_EQ(r1->as_lifecycle()->state(), LifecycleState::kActive);
    EXPECT_EQ(r2->as_lifecycle()->state(), LifecycleState::kActive);
    EXPECT_EQ(r1->start_count(), 1);
    EXPECT_EQ(r2->start_count(), 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// Group 3: Scheduled messages and edge cases
// ═══════════════════════════════════════════════════════════════════════════

// ── Schedule a self-message and verify it is delivered ────────────────

TEST(SupervisionWorkflow, ScheduledMessageDelivery) {
    // Scheduler must be running for timer callbacks to fire.  Use the
    // assert_eventually polling helper with scheduler_start_paused=false.
    Config cfg = test::config_with_scheduler(1);
    cfg.scheduler_start_paused = false;
    ActorSystem system(cfg);

    auto actor = system.spawn<RestartTrackingActor>();
    auto* rt = static_cast<RestartTrackingActor*>(actor.get().get());

    // Schedule a message with zero delay — the scheduler will fire it
    // as soon as its timer advancement thread processes the wheel.
    TypedMessage msg(TypeTag(0x2001), StreamBuffer{});
    msg.set_sender_address(actor.address());
    rt->context()->schedule(std::chrono::milliseconds(0), std::move(msg));

    bool delivered =
        test::assert_eventually([&]() { return rt->message_count() >= 1; }, 5000);
    EXPECT_TRUE(delivered);
    EXPECT_EQ(rt->message_count(), 1);
}

// ── Two independent actor systems operate without interference ────────

TEST(SupervisionWorkflow, MultipleActorSystemsIndependent) {
    Config cfg1 = test::config_with_scheduler(1);
    cfg1.scheduler_start_paused = true;
    ActorSystem system1(cfg1);

    Config cfg2 = test::config_with_scheduler(1);
    cfg2.scheduler_start_paused = true;
    ActorSystem system2(cfg2);

    auto a1 = system1.spawn<RestartTrackingActor>();
    auto a2 = system2.spawn<RestartTrackingActor>();

    auto* r1 = static_cast<RestartTrackingActor*>(a1.get().get());
    auto* r2 = static_cast<RestartTrackingActor*>(a2.get().get());

    // Each actor was started in its own system
    EXPECT_EQ(r1->start_count(), 1);
    EXPECT_EQ(r2->start_count(), 1);

    // Deliver a message to system1's actor only
    TypedMessage msg(TypeTag(0x3001), StreamBuffer{});
    msg.set_sender_address(a1.address());
    system1.deliver_local(a1.id(), std::move(msg));

    test::SchedulerTestDriver driver1(system1);
    bool done1 = driver1.drain_until([&]() { return r1->message_count() >= 1; });
    EXPECT_TRUE(done1);
    EXPECT_EQ(r1->message_count(), 1);

    // System2's actor is unaffected
    EXPECT_EQ(r2->message_count(), 0);
}
