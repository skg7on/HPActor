// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#include <hpactor/sched/actor_execution_engine.hpp>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/types/failure_reason.hpp>

#include <chrono>

#if HPACTOR_SUPPORT_COROUTINES
#    include <hpactor/sched/coroutine_task.hpp>
#endif

namespace hpactor::sched {

namespace {

uint64_t steady_now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void emit_expired_metric(const ActorExecutionContext& context, ActorId actor,
                         uint64_t now_ns) noexcept {
    if (!context.metrics) {
        return;
    }
    metrics::MetricEvent evt{};
    evt.timestamp_ns = now_ns;
    evt.actor_id = actor;
    evt.event_type = metrics::MetricEventType::kDeliveryExpired;
    evt.code = static_cast<uint8_t>(FailureReason::Expired);
    evt.value_hi = 1;
    context.metrics->try_push(evt);
}

} // namespace

BehaviorActorRunner::BehaviorActorRunner(ActorSystem& system,
                                         ActorReadyGate& ready_gate) noexcept
    : system_(system), ready_gate_(ready_gate) {}

ActorRunResult
BehaviorActorRunner::run(EventBasedActor& actor, const WorkItem& item,
                         const ActorExecutionContext& context) noexcept {
    auto& actor_state = actor.actor_state();

    uint32_t expected = ActorState::kReady;
    if (!actor_state.cas(expected, ActorState::kRunning)) {
        if (actor_state.is_terminated()) {
            actor.set_exit_reason(errors::actor_down);
            actor.on_exit();
            return {ActorRunDisposition::Terminated, 0, INT64_MAX};
        }
        return {ActorRunDisposition::Skipped, 0, INT64_MAX};
    }

    auto mailbox = system_.get_mailbox(item.actor);
    if (!mailbox) {
        actor_state.set(ActorState::kIdle);
        return {ActorRunDisposition::Skipped, 0, INT64_MAX};
    }

    TypedMessage msg;
    if (mailbox->try_pop(msg)) {
        uint64_t now_ns = steady_now_ns();
        if (mailbox::is_expired(msg.deadline_ns(), now_ns)) {
            emit_expired_metric(context, item.actor, now_ns);
        } else {
            actor.receive(msg);
        }
    }

    if (!mailbox->empty()) {
        auto admission = ready_gate_.mark_ready_already_admitted(actor);
        if (admission.accepted() ||
            admission.code == ReadyAdmissionCode::AlreadyReady) {
            return {ActorRunDisposition::RequeueReady, 0, INT64_MAX};
        }
        return {ActorRunDisposition::Skipped, 0, INT64_MAX};
    }

    actor_state.set(ActorState::kIdle);
    if (!mailbox->empty()) {
        auto admission = ready_gate_.try_mark_ready(item.actor);
        if (admission.accepted() ||
            admission.code == ReadyAdmissionCode::AlreadyReady) {
            return {ActorRunDisposition::RequeueReady, 0, INT64_MAX};
        }
    }

    return {ActorRunDisposition::SuspendedOrIdle, 0, INT64_MAX};
}

#if HPACTOR_SUPPORT_COROUTINES
CoroutineActorRunner::CoroutineActorRunner(ActorSystem& system) noexcept
    : system_(system) {}

ActorRunResult
CoroutineActorRunner::run(EventBasedActor& actor, const WorkItem& item,
                          const ActorExecutionContext& context) noexcept {
    (void)item;
    (void)context;
    (void)system_;

    actor.ensure_coroutine_started();

    auto& coroutine = actor.get_actor_coroutine();
    if (!coroutine) {
        return {ActorRunDisposition::Skipped, 0, INT64_MAX};
    }

    auto& promise = coroutine.task().handle().promise();
    if (promise.actor_state->is_idle() || promise.actor_state->is_io_waiting()) {
        promise.actor_state->set(ActorState::kReady);
    }

    uint32_t expected = ActorState::kReady;
    if (!promise.actor_state->cas(expected, ActorState::kRunning)) {
        if (promise.actor_state->is_terminated()) {
            actor.set_exit_reason(errors::actor_down);
            actor.on_exit();
            return {ActorRunDisposition::Terminated, 0, INT64_MAX};
        }
        return {ActorRunDisposition::Skipped, 0, INT64_MAX};
    }

    coroutine.resume();

    if (coroutine.done()) {
        actor.on_exit();
        return {ActorRunDisposition::Terminated, 0, INT64_MAX};
    }

    return {ActorRunDisposition::SuspendedOrIdle, 0, INT64_MAX};
}
#endif

ActorExecutionEngine::ActorExecutionEngine(ActorSystem& system,
                                           ActorReadyGate& ready_gate) noexcept
    : behavior_runner_(system, ready_gate)
#if HPACTOR_SUPPORT_COROUTINES
      ,
      coroutine_runner_(system)
#endif
{
}

ActorRunResult
ActorExecutionEngine::run(EventBasedActor& actor, const WorkItem& item,
                          const ActorExecutionContext& context,
                          bool use_coroutines) noexcept {
#if HPACTOR_SUPPORT_COROUTINES
    if (use_coroutines) {
        return coroutine_runner_.run(actor, item, context);
    }
#else
    (void)use_coroutines;
#endif
    return behavior_runner_.run(actor, item, context);
}

} // namespace hpactor::sched
