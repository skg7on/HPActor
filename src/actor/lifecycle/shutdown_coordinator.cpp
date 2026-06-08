// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/shutdown_coordinator.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <thread>

namespace hpactor {

ShutdownCoordinator::ShutdownCoordinator(ShutdownCoordinatorDependencies deps)
    : deps_(std::move(deps)) {}

void ShutdownCoordinator::set_phase(ShutdownPhase phase) noexcept {
    if (deps_.phase) {
        deps_.phase->store(phase, std::memory_order_release);
    }
}

ShutdownPhase ShutdownCoordinator::phase() const noexcept {
    if (deps_.phase) {
        return deps_.phase->load(std::memory_order_acquire);
    }
    return ShutdownPhase::Running;
}

bool ShutdownCoordinator::accepting_ingress() const noexcept {
    return phase() == ShutdownPhase::Running;
}

void ShutdownCoordinator::initiate_actor_drain(ActorId id) {
    if (!deps_.get_actor)
        return;
    auto actor = deps_.get_actor(id);
    if (!actor)
        return;

    auto* lc = actor->as_lifecycle();
    if (lc == nullptr) {
        if (actor->is_event_based_actor()) {
            static_cast<EventBasedActor*>(actor.get())->on_exit();
        }
        return;
    }

    auto state = lc->state();
    if (state == LifecycleState::kStopping || state == LifecycleState::kStopped)
        return;

    auto policy = lc->drain_config().policy;

    if (policy == DrainPolicy::ImmediateStop) {
        if (actor->is_event_based_actor()) {
            static_cast<EventBasedActor*>(actor.get())->drain_all_immediate();
        } else {
            auto raw_mbox =
                deps_.get_mailbox_raw ? deps_.get_mailbox_raw(id) : nullptr;
            if (raw_mbox) {
                auto* mailbox =
                    static_cast<mailbox::MPSCActorMailbox<TypedMessage>*>(raw_mbox);
                TypedMessage msg;
                while (mailbox->try_pop(msg)) {
                }
            }
        }
        lc->transition(LifecycleState::kStopping);
        lc->transition(LifecycleState::kStopped);
        if (actor->is_event_based_actor()) {
            static_cast<EventBasedActor*>(actor.get())->on_exit();
        }
    } else {
        if (state == LifecycleState::kActive) {
            lc->transition(LifecycleState::kDraining);
        }
        if (actor->is_event_based_actor()) {
            static_cast<EventBasedActor*>(actor.get())->start_drain_timer();
        } else {
            lc->transition(LifecycleState::kStopping);
            lc->transition(LifecycleState::kStopped);
        }
    }
}

void ShutdownCoordinator::poll_drain_complete(
    ActorId id, std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        if (!deps_.get_actor)
            return;
        auto actor = deps_.get_actor(id);
        if (!actor)
            return;

        auto* lc = actor->as_lifecycle();
        if (lc == nullptr)
            return;

        if (lc->state() == LifecycleState::kStopped)
            return;

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ShutdownCoordinator::execute(const ShutdownOptions& opts) {
    auto check_force = [&](std::chrono::steady_clock::time_point deadline) -> bool {
        if (!opts.force_after_timeout)
            return false;
        if (std::chrono::steady_clock::now() < deadline)
            return false;
        set_phase(ShutdownPhase::ForcedStop);
        if (deps_.running)
            deps_.running->store(false, std::memory_order_release);
        return true;
    };

    set_phase(ShutdownPhase::DrainingIngress);
    if (deps_.set_ready)
        deps_.set_ready(false);
    auto ingress_deadline = std::chrono::steady_clock::now() + opts.ingress_timeout;
    if (check_force(ingress_deadline))
        return;

    set_phase(ShutdownPhase::DrainingActors);
    auto actor_deadline =
        std::chrono::steady_clock::now() + opts.actor_drain_timeout;

    std::vector<std::pair<ActorId, bool>> actors;
    if (deps_.actor_snapshot)
        actors = deps_.actor_snapshot();

    // Pass 1: drain non-system actors
    for (auto& [id, is_sys] : actors) {
        if (is_sys)
            continue;
        (void)is_sys;
        initiate_actor_drain(id);
        if (check_force(actor_deadline))
            return;
    }
    if (!check_force(actor_deadline)) {
        for (const auto& [id, is_sys] : actors) {
            if (is_sys)
                continue;
            poll_drain_complete(id, actor_deadline);
            if (check_force(actor_deadline))
                return;
        }
    }

    // Pass 2: drain system actors
    if (!check_force(actor_deadline)) {
        for (auto& [id, is_sys] : actors) {
            if (!is_sys)
                continue;
            (void)is_sys;
            initiate_actor_drain(id);
            if (check_force(actor_deadline))
                return;
        }
    }
    if (!check_force(actor_deadline)) {
        for (const auto& [id, is_sys] : actors) {
            if (!is_sys)
                continue;
            poll_drain_complete(id, actor_deadline);
            if (check_force(actor_deadline))
                return;
        }
    }
    if (check_force(actor_deadline))
        return;

    set_phase(ShutdownPhase::LeavingCluster);
    auto leave_deadline =
        std::chrono::steady_clock::now() + opts.cluster_leave_timeout;
    if (deps_.leave_discovery)
        deps_.leave_discovery();
    if (deps_.stop_remote_runtime)
        deps_.stop_remote_runtime();
    if (check_force(leave_deadline))
        return;

    set_phase(ShutdownPhase::FlushingTelemetry);
    if (deps_.flush_telemetry)
        deps_.flush_telemetry();

    set_phase(ShutdownPhase::Stopped);
    if (deps_.running)
        deps_.running->store(false, std::memory_order_release);
}

} // namespace hpactor
