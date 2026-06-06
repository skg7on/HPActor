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

#include <hpactor/mailbox/detail/overflow_handler_interface.hpp>

namespace hpactor::mailbox::detail {

/// \brief Overflow handler for \c OverflowPolicy::DeadLetter.
///
/// Increments the dead-letter counter, emits a \c kMailboxDeadLetter metric
/// event, and returns \c ReroutedToDeadLetter. The message is assumed to have
/// been consumed by an external dead-letter queue — this handler only records
/// the routing decision.
///
/// \tparam T Message type stored in the mailbox.
template <typename T> class DeadLetterHandler : public IOverflowHandler<T> {
  public:
    /// \brief Handle a reservation failure by routing to the dead-letter queue.
    ///
    /// \param[in,out] ctx Overflow context with counters and metrics buffer.
    /// \param reason Reservation failure reason (unused).
    /// \return \c ReroutedToDeadLetter with current mailbox state.
    EnqueueResult
    handle(OverflowContext<T>& ctx, ReservationResult /*reason*/) override {
        ctx.total_dead_letters.fetch_add(1, std::memory_order_relaxed);
        if (ctx.metrics_buf) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = ctx.actor_id;
            evt.event_type = metrics::MetricEventType::kMailboxDeadLetter;
            evt.value_hi = 1;
            ctx.metrics_buf->try_push(evt);
        }
        EnqueueResult r;
        r.code = EnqueueResultCode::ReroutedToDeadLetter;
        r.target = ctx.actor_id;
        r.depth = ctx.current_depth;
        r.capacity = ctx.config.capacity.max_messages;
        r.bytes = ctx.current_bytes;
        r.byte_capacity = ctx.config.capacity.max_bytes;
        r.pressure_reason = BackpressureReason::OverflowPolicy;
        return r;
    }

    OverflowPolicy policy() const override {
        return OverflowPolicy::DeadLetter;
    }
};

} // namespace hpactor::mailbox::detail
