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

#include <hpactor/adt/id.hpp>
#include <hpactor/adt/tags.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <functional>

namespace hpactor::sched {

/// \brief Opaque handle for a scheduled timer.
using TimerHandle = Id<TimerTag>;

/// \brief Callback invoked when a timer fires.
using timer_callback = std::function<void()>;

/// \brief Narrow interface for notifying the scheduler that an actor is
///        ready to run.
///
/// Used by awaiters, mailbox enqueue paths, and timer callbacks to signal
/// readiness without depending on the full scheduler.
///
/// \note Thread safety: safe from any thread.
class IActorReadyNotifier {
  public:
    virtual ~IActorReadyNotifier() = default;

    /// \brief Notify that an actor has work and should be scheduled.
    ///
    /// \param[in] actor Actor ID.
    /// \param[in] priority 0 = highest priority.
    /// \param[in] deadline_ns Absolute deadline in nanoseconds, or
    ///                       \c INT64_MAX for no deadline.
    virtual void
    notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;
};

/// \brief Narrow interface for timer scheduling and cancellation.
///
/// Used by \c TimerAwaiter and other timer-dependent code to avoid
/// depending on the full scheduler.
///
/// \note Thread safety: safe from any thread. Timer callbacks must not
///       block.
class ITimerService {
  public:
    virtual ~ITimerService() = default;

    /// \brief Schedule a one-shot timer.
    ///
    /// \param[in] cb Callback invoked when the timer fires.
    /// \param[in] delay_ns Delay in nanoseconds from now.
    /// \return Handle that can be used to cancel the timer.
    virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;

    /// \brief Cancel a previously scheduled timer.
    ///
    /// \param[in] handle Timer handle returned by \c schedule_after().
    virtual void cancel_timer(TimerHandle handle) = 0;
};

/// \brief Narrow interface for cooperative actor yield.
///
/// Used by \c YieldAwaiter to re-enqueue a running actor without
/// depending on the full scheduler.
///
/// \note Thread safety: must be called from within the actor's own
///       execution context (the worker thread running the actor).
class IActorYieldScheduler {
  public:
    virtual ~IActorYieldScheduler() = default;

    /// \brief Voluntarily yield the current actor, re-enqueuing it at the
    ///        given priority.
    ///
    /// \param[in] actor Actor ID.
    /// \param[in] priority Priority level for re-enqueue.
    virtual void yield(ActorId actor, uint8_t priority) = 0;
};

} // namespace hpactor::sched
