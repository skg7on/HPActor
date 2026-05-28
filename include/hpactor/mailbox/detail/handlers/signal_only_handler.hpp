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

#include <chrono>

namespace hpactor::mailbox::detail {

template <typename T> class SignalOnlyHandler : public IOverflowHandler<T> {
  public:
    EnqueueResult
    handle(OverflowContext<T>& ctx, ReservationResult reason) override {
        ctx.total_rejected.fetch_add(1, std::memory_order_relaxed);
        if (ctx.metrics_buf) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = ctx.actor_id;
            evt.event_type = metrics::MetricEventType::kMailboxRejected;
            evt.value_hi = 1;
            ctx.metrics_buf->try_push(evt);
        }
        EnqueueResult r;
        r.code = EnqueueResultCode::Rejected;
        r.target = ctx.actor_id;
        r.depth = ctx.current_depth;
        r.capacity = ctx.config.capacity.max_messages;
        r.bytes = ctx.current_bytes;
        r.byte_capacity = ctx.config.capacity.max_bytes;
        r.pressure_reason = reason == ReservationResult::ByteCapacity
                                ? BackpressureReason::ByteCapacity
                                : BackpressureReason::HardCapacity;
        r.retry_after =
            std::chrono::milliseconds(ctx.config.signal_min_interval_ms);
        return r;
    }

    OverflowPolicy policy() const override {
        return OverflowPolicy::SignalOnly;
    }
};

} // namespace hpactor::mailbox::detail
