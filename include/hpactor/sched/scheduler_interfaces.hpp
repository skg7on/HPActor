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
#include <hpactor/adt/id.hpp>
#include <hpactor/adt/tags.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <functional>

namespace hpactor::sched {

/// \brief Opaque handle for a scheduled timer.
///
/// Encodes shard index, slot index, generation, and type tag in 64 bits
/// for O(1) slot-array lookup in TimerPlane.  Backward-compatible with
/// the plain Id<TimerTag> interface for TimingWheel/CalendarQueue use.
class TimerHandle : public Id<TimerTag> {
  public:
    using Id<TimerTag>::Id;

    /// Construct from encoded fields.
    static TimerHandle make_encoded(uint32_t shard_index, uint32_t slot_index,
                                    uint8_t generation, uint16_t type_tag) {
        uint64_t v = (static_cast<uint64_t>(type_tag) << 48) |
                     (static_cast<uint64_t>(shard_index) << 32) |
                     (static_cast<uint64_t>(generation) << 24) |
                     static_cast<uint64_t>(slot_index);
        return TimerHandle{v};
    }

    /// Decode helpers.
    static uint32_t slot_index(TimerHandle h) {
        return static_cast<uint32_t>(h.value() & 0xFFFFFFULL);
    }
    static uint8_t generation(TimerHandle h) {
        return static_cast<uint8_t>((h.value() >> 24) & 0xFFULL);
    }
    static uint32_t shard_index(TimerHandle h) {
        return static_cast<uint32_t>((h.value() >> 32) & 0xFFFFULL);
    }
    static uint16_t type_tag(TimerHandle h) {
        return static_cast<uint16_t>((h.value() >> 48) & 0xFFFFULL);
    }
};

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

    /// \brief Notify that an actor has work and should be scheduled with EDF
    ///        (Earliest Deadline First) semantics.
    ///
    /// The work item is placed in the worker's EDFQueue instead of the
    /// priority ChaseLev deque.  Used for messages enqueued via
    /// \c deliver_local_edf() or \c send_edf().
    ///
    /// \param[in] actor Actor ID.
    /// \param[in] priority Priority level (used as a tiebreaker within the
    ///                     same deadline bucket).
    /// \param[in] deadline_ns Absolute deadline in nanoseconds. Must not
    ///                       be \c INT64_MAX.
    virtual void
    notify_ready_edf(ActorId actor, uint8_t priority, int64_t deadline_ns) {
        // Default: fall back to regular notify_ready (EDF-unaware schedulers).
        notify_ready(actor, priority, deadline_ns);
    }

    /// \brief Fast-path wakeup with direct actor pointer.
    ///
    /// Called by MPSCActorMailbox when actor_ptr is known, allowing the
    /// scheduler to populate WorkItem::actor_ptr and skip the get_actor()
    /// hash lookup in execute_actor().
    ///
    /// Default implementation ignores the pointer and delegates to
    /// notify_ready(), preserving backward compatibility for all existing
    /// scheduler mocks and stub implementations.
    ///
    /// \param[in] actor     Actor ID.
    /// \param[in] actor_ptr Direct pointer to the EventBasedActor.
    /// \param[in] priority  0 = highest priority.
    /// \param[in] deadline_ns Deadline in nanoseconds or INT64_MAX.
    virtual void notify_ready_fast(ActorId actor, EventBasedActor* /*actor_ptr*/,
                                   uint8_t priority, int64_t deadline_ns) {
        notify_ready(actor, priority, deadline_ns);
    }
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
