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

#include <hpactor/mailbox/detail/pressure_state_machine.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>

namespace hpactor::net {

struct EndpointOutboundLimits {
    size_t max_messages = 1000;
    size_t max_bytes = 16 * 1024 * 1024; // 16 MiB
    size_t control_lane_reserve = 64;
    double reliable_headroom_pct = 0.20;
    double high_watermark = 0.70;
    double critical_watermark = 0.90;
    double low_watermark = 0.50;
    double drain_rate_ema_alpha = 0.20;
};

struct EndpointOutboundCounts {
    std::atomic<size_t> control_messages{0};
    std::atomic<size_t> control_bytes{0};
    std::atomic<size_t> data_messages{0};
    std::atomic<size_t> data_bytes{0};
};

class EndpointOutboundQueue {
  public:
    explicit EndpointOutboundQueue(const EndpointOutboundLimits& limits);

    mailbox::EnqueueResult
    try_enqueue(PendingMessage msg, mailbox::DeliveryMode mode, TypeTag type_tag);

    std::optional<PendingMessage> try_dequeue();

    EndpointOutboundCounts snapshot() const;

    mailbox::MailboxPressureState pressure_state() const;

    double depth_ratio() const;

    size_t total_messages() const;
    size_t total_bytes() const;
    size_t control_messages() const;
    size_t data_messages() const;

  private:
    bool check_admission(size_t msg_bytes, bool is_control,
                         mailbox::DeliveryMode mode) const;
    void update_pressure_after_enqueue();
    void update_pressure_after_dequeue(size_t bytes_dequeued);

    EndpointOutboundLimits limits_;
    EndpointOutboundCounts counts_;
    std::deque<PendingMessage> control_lane_;
    std::deque<PendingMessage> data_lane_;
    mailbox::detail::PressureStateMachine pressure_;
    std::atomic<double> drain_rate_ema_{0.0};
    std::atomic_flag spinlock_ = ATOMIC_FLAG_INIT;
};

} // namespace hpactor::net
