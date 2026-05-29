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
#include <hpactor/types/types.hpp>

#include <cstdint>

namespace hpactor {
class ActorSystem;
class EventBasedActor;
} // namespace hpactor

namespace hpactor::sched {

/// \brief Result of a readiness admission attempt on an event-based actor.
enum class ReadyAdmissionCode : uint8_t {
    Accepted,       ///< Actor was Idle or IOWaiting and is now Ready.
    MissingActor,   ///< Actor ID not found in the actor system.
    AlreadyReady,   ///< Actor is already enqueued — duplicate notification.
    AlreadyRunning, ///< Actor is currently executing on a worker.
    Terminated,     ///< Actor has reached the terminated state.
    NotAdmissible,  ///< Actor is in a state that does not admit readiness.
};

/// \brief Opaque result of an admission gate check.
///
/// Callers test \c accepted() rather than inspecting the code directly.
struct ReadyAdmission {
    ReadyAdmissionCode code{ReadyAdmissionCode::NotAdmissible};

    /// \return \c true if the actor was successfully transitioned to Ready.
    bool accepted() const noexcept {
        return code == ReadyAdmissionCode::Accepted;
    }
};

/// \brief CAS-based readiness admission gate for event-based actors.
///
/// Prevents duplicate Ready/Running/Terminated actors from being enqueued
/// in the scheduler. Only Idle and IOWaiting actors pass the public
/// admission gate. Actors that already own the activation (requeue, yield)
/// use \c mark_ready_already_admitted() which accepts Running and Idle
/// transitions.
///
/// \note Thread safety: all methods are safe from any thread. The
///       underlying \c ActorState CAS provides the synchronization
///       guarantee.
class ActorReadyGate {
  public:
    /// \brief Construct the gate bound to an actor system.
    ///
    /// \param[in] system Actor system used for actor lookup.
    explicit ActorReadyGate(ActorSystem& system) noexcept;

    /// \brief Try to admit an actor into the Ready state from an external
    ///        notification (mailbox enqueue, timer expiry, I/O event).
    ///
    /// Accepts only Idle and IOWaiting actors. Rejects Ready, Running,
    /// and Terminated actors.
    ///
    /// \param[in] actor Actor to transition.
    /// \return Admission result with the outcome code.
    ReadyAdmission try_mark_ready(ActorId actor) noexcept;

    /// \brief Mark an actor Ready when the caller already owns the
    ///        activation (requeue after processing one message, coroutine
    ///        yield, lost-wakeup double-check).
    ///
    /// Accepts Running, Idle, and IOWaiting transitions. This is not a
    /// public entry point — it is intended for actor execution code that
    /// has already proven ownership of the actor activation.
    ///
    /// \param[in] actor Event-based actor reference.
    /// \return Admission result. \c Accepted means the actor is now Ready.
    ReadyAdmission mark_ready_already_admitted(EventBasedActor& actor) noexcept;

  private:
    ActorSystem& system_;
};

} // namespace hpactor::sched
