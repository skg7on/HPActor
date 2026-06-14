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

#include <hpactor/sched/actor_execution_engine.hpp>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/failure_envelope.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <chrono>

#if HPACTOR_SUPPORT_COROUTINES
#    include <hpactor/coroutine/coroutine_task.hpp>
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

            auto* dlq = system_.dead_letter_queue();
            if (dlq && dlq->config().enabled) {
                hpactor::mailbox::DeadLetterRecord dl;
                dl.reason = hpactor::mailbox::DeadLetterReason::Expired;
                dl.source = hpactor::mailbox::DeadLetterSource::LocalDelivery;
                dl.sender = msg.sender_address();
                dl.target = actor.address();
                dl.type_tag = msg.type_id();
                dl.deadline_ns = msg.deadline_ns();
                dl.payload_sample = msg.payload();
                dl.timestamp_ns = now_ns;
                if (msg.has_trace_context()) {
                    auto& tc = msg.trace_context();
                    std::memcpy(&dl.trace_id_hi, tc.trace_id.bytes.data(), 8);
                    std::memcpy(&dl.trace_id_lo, tc.trace_id.bytes.data() + 8, 8);
                    std::memcpy(&dl.span_id, tc.span_id.bytes.data(), 8);
                }
                (void)dlq->try_push(std::move(dl));
            }
        } else {
            actor.receive(msg);
        }
    } else {
        // try_pop returned nullptr (MPSC chain in progress or genuinely
        // empty).  Set the actor idle, then double-check: if the mailbox
        // became non-empty in the window between the dequeue and kIdle
        // (producer completed enqueue + notify_ready saw kRunning on
        // x86_64 TSO), re-admit via try_mark_ready so the message is
        // not orphaned.  count_.load(acquire) synchronises with
        // count_.fetch_add(1, release), guaranteeing mpsc_next
        // visibility when count_ > 0 on all architectures.
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

    // Cap RequeueReady cycles to prevent one high-traffic actor from
    // monopolising a worker.  The budget is carried in item.sequence
    // (incremented by execute_actor on each RequeueReady round-trip).
    // After kRequeueBudget consecutive cycles, force the actor to kIdle
    // so other actors get a chance to run.  If the mailbox still has
    // messages the double-check below will re-admit the actor via
    // try_mark_ready (external notification path), naturally interleaving
    // it with other work.
    // Skip the cap when workers are paused (deterministic test mode).
    static constexpr uint64_t kRequeueBudget = 64;
    bool budget_exhausted =
        !context.workers_paused && item.sequence >= kRequeueBudget;

    if (!mailbox->empty() && !budget_exhausted) {
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
