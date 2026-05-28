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
#include <hpactor/mailbox/mailbox_policy.hpp>

namespace hpactor::mailbox::detail {

template <typename T> class RejectNewestHandler : public IOverflowHandler<T> {
  public:
    EnqueueResult
    handle(OverflowContext<T>& ctx, ReservationResult reason) override {
        ctx.total_rejected.fetch_add(1, std::memory_order_relaxed);
        emit_metric(ctx, metrics::MetricEventType::kMailboxRejected);
        return build_result(ctx, EnqueueResultCode::Rejected,
                            reason == ReservationResult::ByteCapacity
                                ? BackpressureReason::ByteCapacity
                                : BackpressureReason::HardCapacity);
    }

    OverflowPolicy policy() const override {
        return OverflowPolicy::RejectNewest;
    }

  private:
    static void
    emit_metric(OverflowContext<T>& ctx, metrics::MetricEventType type) {
        if (ctx.metrics_buf) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = ctx.actor_id;
            evt.event_type = type;
            evt.value_hi = 1;
            ctx.metrics_buf->try_push(evt);
        }
    }

    static EnqueueResult
    build_result(OverflowContext<T>& ctx, EnqueueResultCode code,
                 BackpressureReason reason) {
        EnqueueResult r;
        r.code = code;
        r.target = ctx.actor_id;
        r.depth = ctx.current_depth;
        r.capacity = ctx.config.capacity.max_messages;
        r.bytes = ctx.current_bytes;
        r.byte_capacity = ctx.config.capacity.max_bytes;
        r.pressure_reason = reason;
        return r;
    }
};

} // namespace hpactor::mailbox::detail
