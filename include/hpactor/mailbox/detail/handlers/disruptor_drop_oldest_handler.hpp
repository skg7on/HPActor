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

namespace hpactor::mailbox::detail {

/// \brief Overflow handler for \c OverflowPolicy::DropOldest.
///
/// Attempts to evict the oldest user message from the highest-priority
/// non-empty ring via \c DisruptorOverflowContext::drop_oldest_fn.
/// On success, returns \c DroppedExisting so the producer retries
/// reservation. On failure (all rings empty), returns \c Rejected.
///
/// \tparam EnvelopeType The disruptor message envelope type.
template <typename EnvelopeType>
class DisruptorDropOldestHandler : public IDisruptorOverflowHandler<EnvelopeType> {
  public:
    EnqueueResult handle(DisruptorOverflowContext<EnvelopeType>& ctx,
                         adt::ReservationResult reason) override {
        if (ctx.drop_oldest_fn && ctx.drop_oldest_fn()) {
            EnqueueResult r;
            r.code = EnqueueResultCode::DroppedExisting;
            r.target = ctx.actor_id;
            r.depth = ctx.current_depth;
            r.capacity = ctx.config.capacity.max_messages;
            r.bytes = ctx.current_bytes;
            r.byte_capacity = ctx.config.capacity.max_bytes;
            r.pressure_reason = BackpressureReason::OverflowPolicy;
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
        return OverflowPolicy::DropOldest;
    }
};

} // namespace hpactor::mailbox::detail
