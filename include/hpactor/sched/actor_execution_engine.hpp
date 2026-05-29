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

#pragma once

#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/actor_ready_gate.hpp>
#include <hpactor/sched/work_queue.hpp>

#include <cstdint>

namespace hpactor {
class ActorSystem;
class EventBasedActor;
} // namespace hpactor

namespace hpactor::sched {

/// \brief Outcome of a single actor activation.
enum class ActorRunDisposition : uint8_t {
    Skipped,         ///< Actor was not run (wrong state, missing mailbox).
    SuspendedOrIdle, ///< Actor ran and is now idle or suspended (coroutine).
    RequeueReady,    ///< Actor still has work — caller must re-enqueue.
    Terminated,      ///< Actor reached terminal state; \c on_exit() was called.
};

/// \brief Result of running an actor activation.
///
/// When \c disposition is \c RequeueReady, the caller should call
/// \c enqueue_admitted() with the returned \c priority and \c deadline_ns.
struct ActorRunResult {
    ActorRunDisposition disposition{ActorRunDisposition::Skipped};
    uint8_t priority{0};            ///< Priority for requeue.
    int64_t deadline_ns{INT64_MAX}; ///< Deadline for requeue.
};

/// \brief Execution context passed from the scheduler facade to the
///        execution engine.
struct ActorExecutionContext {
    uint32_t worker_id{UINT32_MAX};
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics{nullptr};
    log::Logger* logger{nullptr};
};

/// \brief Runs one activation of a behavior-based event actor.
///
/// Pops at most one mailbox message, checks deadline expiry, calls
/// \c actor.receive(), and decides whether to requeue based on remaining
/// mailbox work. Owns the \c Ready \c → \c Running CAS transition and the
/// lost-wakeup double-check.
///
/// \note Thread safety: at most one caller may invoke \c run() per actor
///       at a time. The \c Ready→Running CAS enforces this.
class BehaviorActorRunner {
  public:
    /// \brief Construct a behavior runner.
    ///
    /// \param[in] system Actor system for mailbox and actor lookup.
    /// \param[in] ready_gate Readiness gate for requeue admission.
    BehaviorActorRunner(ActorSystem& system, ActorReadyGate& ready_gate) noexcept;

    /// \brief Run one behavior activation.
    ///
    /// \param[in] actor Event-based actor to run.
    /// \param[in] item Work item that triggered this activation.
    /// \param[in] context Metrics and logging context.
    /// \return Disposition: \c RequeueReady if the mailbox still has work,
    ///         \c SuspendedOrIdle if the actor is done for now,
    ///         \c Terminated if the actor reached the terminal state.
    ActorRunResult run(EventBasedActor& actor, const WorkItem& item,
                       const ActorExecutionContext& context) noexcept;

  private:
    ActorSystem& system_;
    ActorReadyGate& ready_gate_;
};

#if HPACTOR_SUPPORT_COROUTINES
/// \brief Runs one activation of a coroutine-based event actor.
///
/// Lazily starts the coroutine on first invocation, transitions
/// \c Ready → \c Running, resumes the coroutine, and observes the
/// post-resume state (\c Idle, \c IOWaiting, or terminated).
///
/// \note Thread safety: at most one caller may invoke \c run() per actor
///       at a time. This runner does not choose a worker or push to any
///       worker queue — wakeups are readiness-based.
class CoroutineActorRunner {
  public:
    /// \brief Construct a coroutine runner.
    ///
    /// \param[in] system Actor system for coroutine lifecycle management.
    explicit CoroutineActorRunner(ActorSystem& system) noexcept;

    /// \brief Run one coroutine activation.
    ///
    /// \param[in] actor Event-based actor to run.
    /// \param[in] item Work item that triggered this activation (unused).
    /// \param[in] context Metrics and logging context (unused).
    /// \return \c SuspendedOrIdle if the coroutine suspended,
    ///         \c Terminated if the coroutine completed.
    ActorRunResult run(EventBasedActor& actor, const WorkItem& item,
                       const ActorExecutionContext& context) noexcept;

  private:
    ActorSystem& system_;
};
#endif

/// \brief Actor execution engine that dispatches to behavior or coroutine
///        runners.
///
/// Owns the activation decision after a worker has selected a \c WorkItem.
/// The engine emits scheduler dispatch metrics, sets memory accounting,
/// selects the execution mode, and returns a disposition that the caller
/// uses to decide whether to requeue.
///
/// \note Thread safety: \c run() is called by at most one worker per
///       admitted \c WorkItem. The underlying runners enforce the per-actor
///       mutual exclusion via the \c Ready→Running CAS.
class ActorExecutionEngine {
  public:
    /// \brief Construct the execution engine.
    ///
    /// \param[in] system Actor system for actor and mailbox lookup.
    /// \param[in] ready_gate Readiness gate shared with the scheduler.
    ActorExecutionEngine(ActorSystem& system, ActorReadyGate& ready_gate) noexcept;

    /// \brief Run one actor activation, selecting behavior or coroutine
    ///        mode.
    ///
    /// \param[in] actor Event-based actor to run.
    /// \param[in] item Work item that triggered this activation.
    /// \param[in] context Metrics, logging, and worker identity.
    /// \param[in] use_coroutines If \c true and coroutine support is
    ///            compiled in, use the coroutine runner.
    /// \return Disposition for the caller to act on.
    ActorRunResult
    run(EventBasedActor& actor, const WorkItem& item,
        const ActorExecutionContext& context, bool use_coroutines) noexcept;

  private:
    BehaviorActorRunner behavior_runner_;
#if HPACTOR_SUPPORT_COROUTINES
    CoroutineActorRunner coroutine_runner_;
#endif
};

} // namespace hpactor::sched
