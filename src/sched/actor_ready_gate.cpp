// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#include <hpactor/sched/actor_ready_gate.hpp>

#include <hpactor/actor/actor_state.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>

namespace hpactor::sched {

ActorReadyGate::ActorReadyGate(ActorSystem& system) noexcept
    : system_(system) {}

ReadyAdmission ActorReadyGate::try_mark_ready(ActorId actor) noexcept {
    auto actor_ptr = system_.get_actor(actor);
    if (!actor_ptr) {
        return {ReadyAdmissionCode::MissingActor};
    }
    if (!actor_ptr->is_event_based_actor()) {
        return {ReadyAdmissionCode::Accepted};
    }

    auto* eb = static_cast<EventBasedActor*>(actor_ptr.get());
    auto& state = eb->actor_state();

    for (;;) {
        uint32_t current = state.get();
        switch (current) {
            case ActorState::kIdle:
            case ActorState::kIOWaiting: {
                uint32_t expected = current;
                if (state.cas(expected, ActorState::kReady)) {
                    return {ReadyAdmissionCode::Accepted};
                }
                continue;
            }
            case ActorState::kReady:
                return {ReadyAdmissionCode::AlreadyReady};
            case ActorState::kRunning:
                return {ReadyAdmissionCode::AlreadyRunning};
            case ActorState::kTerminated:
                return {ReadyAdmissionCode::Terminated};
            default:
                return {ReadyAdmissionCode::NotAdmissible};
        }
    }
}

ReadyAdmission
ActorReadyGate::mark_ready_already_admitted(EventBasedActor& actor) noexcept {
    auto& state = actor.actor_state();
    for (;;) {
        uint32_t current = state.get();
        switch (current) {
            case ActorState::kRunning:
            case ActorState::kIdle:
            case ActorState::kIOWaiting: {
                uint32_t expected = current;
                if (state.cas(expected, ActorState::kReady)) {
                    return {ReadyAdmissionCode::Accepted};
                }
                continue;
            }
            case ActorState::kReady:
                return {ReadyAdmissionCode::AlreadyReady};
            case ActorState::kTerminated:
                return {ReadyAdmissionCode::Terminated};
            default:
                return {ReadyAdmissionCode::NotAdmissible};
        }
    }
}

} // namespace hpactor::sched
