// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#include <gtest/gtest.h>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/sched/actor_ready_gate.hpp>

using namespace hpactor;

namespace {

class ReadyGateActor : public EventBasedActor {
  public:
    ReadyGateActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

ActorSystem make_system() {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    return ActorSystem(cfg);
}

} // namespace

TEST(ActorReadyGateTest, IdleActorIsAdmittedAsReady) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kIdle);

    sched::ActorReadyGate gate(system);
    auto result = gate.try_mark_ready(actor.id());

    EXPECT_TRUE(result.accepted());
    EXPECT_TRUE(eba->actor_state().is_ready());
}

TEST(ActorReadyGateTest, IOWaitingActorIsAdmittedAsReady) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kIOWaiting);

    sched::ActorReadyGate gate(system);
    auto result = gate.try_mark_ready(actor.id());

    EXPECT_TRUE(result.accepted());
    EXPECT_TRUE(eba->actor_state().is_ready());
}

TEST(ActorReadyGateTest, DuplicateReadyActorIsRejected) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kReady);

    sched::ActorReadyGate gate(system);
    auto result = gate.try_mark_ready(actor.id());

    EXPECT_EQ(result.code, sched::ReadyAdmissionCode::AlreadyReady);
    EXPECT_TRUE(eba->actor_state().is_ready());
}

TEST(ActorReadyGateTest, RunningActorIsRejectedForPublicReadiness) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kRunning);

    sched::ActorReadyGate gate(system);
    auto result = gate.try_mark_ready(actor.id());

    EXPECT_EQ(result.code, sched::ReadyAdmissionCode::AlreadyRunning);
    EXPECT_TRUE(eba->actor_state().is_running());
}

TEST(ActorReadyGateTest, RunningActorCanBeMarkedReadyWhenAlreadyAdmitted) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kRunning);

    sched::ActorReadyGate gate(system);
    auto result = gate.mark_ready_already_admitted(*eba);

    EXPECT_TRUE(result.accepted());
    EXPECT_TRUE(eba->actor_state().is_ready());
}

TEST(ActorReadyGateTest, TerminatedActorIsRejected) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kTerminated);

    sched::ActorReadyGate gate(system);
    auto result = gate.try_mark_ready(actor.id());

    EXPECT_EQ(result.code, sched::ReadyAdmissionCode::Terminated);
    EXPECT_TRUE(eba->actor_state().is_terminated());
}
