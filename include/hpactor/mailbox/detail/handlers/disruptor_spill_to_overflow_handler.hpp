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

#include <hpactor/mailbox/detail/disruptor_overflow_handler_interface.hpp>

#include <utility>

namespace hpactor::mailbox::detail {

/// \brief Overflow handler for \c OverflowPolicy::SpillToOverflowQueue.
///
/// Moves the incoming envelope into the overflow queue via
/// \c OverflowQueue::try_push(). On success, returns \c ReroutedToOverflow
/// so the producer knows the message was accepted (albeit deferred).
/// On overflow queue push failure, returns \c Rejected.
///
/// Messages in the overflow queue are drained back into the rings during
/// \c consume_one().
///
/// \tparam EnvelopeType The disruptor message envelope type.
template <typename EnvelopeType>
class DisruptorSpillToOverflowHandler
    : public IDisruptorOverflowHandler<EnvelopeType> {
  public:
    EnqueueResult handle(DisruptorOverflowContext<EnvelopeType>& ctx,
                         adt::ReservationResult reason) override {
        if (ctx.overflow_queue.try_push(EnvelopeType{ctx.envelope})) {
            EnqueueResult r;
            r.code = EnqueueResultCode::ReroutedToOverflow;
            r.target = ctx.actor_id;
            r.depth = ctx.current_depth;
            r.capacity = ctx.config.capacity.max_messages;
            r.bytes = ctx.current_bytes;
            r.byte_capacity = ctx.config.capacity.max_bytes;
            r.pressure_reason = reason == adt::ReservationResult::ByteCapacity
                                    ? BackpressureReason::ByteCapacity
                                    : BackpressureReason::HardCapacity;
            return r;
        }
        ctx.total_rejected.fetch_add(1, std::memory_order_relaxed);
        EnqueueResult r;
        r.code = EnqueueResultCode::Rejected;
        r.target = ctx.actor_id;
        r.depth = ctx.current_depth;
        r.capacity = ctx.config.capacity.max_messages;
        r.bytes = ctx.current_bytes;
        r.byte_capacity = ctx.config.capacity.max_bytes;
        r.pressure_reason = reason == adt::ReservationResult::ByteCapacity
                                ? BackpressureReason::ByteCapacity
                                : BackpressureReason::HardCapacity;
        return r;
    }

    OverflowPolicy policy() const override {
        return OverflowPolicy::SpillToOverflowQueue;
    }
};

} // namespace hpactor::mailbox::detail
