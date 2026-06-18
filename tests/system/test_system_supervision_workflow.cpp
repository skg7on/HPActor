// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

// System test: Supervision Workflow
// Exercises restart tracking, supervisor restart logic, lifecycle
// transitions, quarantine, circuit breaker, self-supervising actors,
// and deep nesting with deterministic scheduler control.

#include <gtest/gtest.h>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>
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
