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

#include <hpactor/mailbox/detail/overflow_context.hpp>
#include <hpactor/mailbox/detail/reservation_manager.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

namespace hpactor::mailbox::detail {

/// \brief Interface for overflow handlers invoked when mailbox capacity
///        reservation fails.
///
/// Each \c OverflowPolicy value maps to a concrete handler via
/// \c make_overflow_handler(). The handler receives an \c OverflowContext
/// with access to the current message, mailbox state, and eviction functions,
/// and returns an \c EnqueueResult describing the outcome.
///
/// \tparam T Message type stored in the mailbox.
///
/// \note Thread safety: \c handle() is called from the producer path
///       under the mailbox's CAS-based admission protocol. Implementations
///       must be safe for concurrent calls from multiple producer threads
///       (typically they only access their \c OverflowContext reference
///       fields, which are thread-local to the calling producer).
template <typename T> class IOverflowHandler {
  public:
    virtual ~IOverflowHandler() = default;

    /// \brief Handle a reservation failure.
    ///
    /// \param[in,out] ctx Overflow context with access to the message,
    ///                    mailbox state, eviction callbacks, and counters.
    /// \param[in] reason Why the reservation failed (count or byte capacity).
    /// \return An \c EnqueueResult describing the outcome (rejected, dropped,
    ///         dead-lettered, spilled, or retry-after-eviction).
    virtual EnqueueResult
    handle(OverflowContext<T>& ctx, ReservationResult reason) = 0;

    /// \brief The \c OverflowPolicy this handler implements.
    ///
    /// \return The corresponding \c OverflowPolicy enum value.
    virtual OverflowPolicy policy() const = 0;
};

} // namespace hpactor::mailbox::detail
