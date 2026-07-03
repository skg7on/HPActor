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

#include <condition_variable>
#include <mutex>

namespace hpactor::mailbox::detail {

/// \brief Overflow handler for \c OverflowPolicy::BlockWhenAllowed.
///
/// Blocks the calling producer thread on a condition variable until
/// capacity becomes available (signalled by the consumer after dequeue).
/// The blocking respects the message deadline — if the deadline expires
/// while blocked, the handler returns \c Rejected with \c Expired reason.
///
/// \tparam EnvelopeType The disruptor message envelope type.
template <typename EnvelopeType>
class DisruptorBlockWhenAllowedHandler
    : public IDisruptorOverflowHandler<EnvelopeType> {
  public:
    EnqueueResult handle(DisruptorOverflowContext<EnvelopeType>& ctx,
                         adt::ReservationResult reason) override {
        // Block until a consumer releases capacity or the deadline expires.
        // The context's block_mutex and block_cv are wired by the mailbox core.
        std::unique_lock<std::mutex> lock(*ctx.block_mutex);
        int64_t deadline_ns = ctx.envelope.meta.deadline_ns;

        while (true) {
            // Check if the reservation can succeed now.
            if (ctx.reservation.try_reserve(sizeof(EnvelopeType),
                                            ctx.config.capacity.max_messages,
                                            ctx.config.capacity.max_bytes) ==
                adt::ReservationResult::Reserved) {
                // Reservation succeeded under the lock — caller will
                // publish. Release so the caller's retry path reserves.
                ctx.reservation.release(sizeof(EnvelopeType));
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

            // Check deadline expiry.
            if (deadline_ns != INT64_MAX) {
                auto now_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
                if (now_ns >= static_cast<uint64_t>(deadline_ns)) {
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
            }

            // Wait for consumer notification.
            ctx.block_cv->wait(lock);
        }
    }

    OverflowPolicy policy() const override {
        return OverflowPolicy::BlockWhenAllowed;
    }
};

} // namespace hpactor::mailbox::detail
