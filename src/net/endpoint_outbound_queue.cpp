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

#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>

namespace hpactor::net {

EndpointOutboundQueue::EndpointOutboundQueue(const EndpointOutboundLimits& limits)
    : limits_(limits) {}

bool EndpointOutboundQueue::check_admission(size_t msg_bytes, bool is_control,
                                            mailbox::DeliveryMode mode) const {
    size_t control_msgs = control_messages_.load(std::memory_order_acquire);
    size_t control_bytes = control_bytes_.load(std::memory_order_acquire);
    size_t data_msgs = data_messages_.load(std::memory_order_acquire);
    size_t data_bytes = data_bytes_.load(std::memory_order_acquire);
    size_t total_msgs = control_msgs + data_msgs;
    size_t total_bytes = control_bytes + data_bytes;

    size_t effective_messages =
        (limits_.max_messages > limits_.control_lane_reserve)
            ? limits_.max_messages - limits_.control_lane_reserve
            : 0;
    double byte_ratio = (limits_.max_messages > 0)
                            ? static_cast<double>(effective_messages) /
                                  static_cast<double>(limits_.max_messages)
                            : 0.0;
    size_t effective_bytes =
        static_cast<size_t>(static_cast<double>(limits_.max_bytes) * byte_ratio);

    if (is_control) {
        if (control_msgs < limits_.control_lane_reserve) {
            return (total_bytes + msg_bytes <= limits_.max_bytes);
        }
        return (total_msgs < limits_.max_messages) &&
               (total_bytes + msg_bytes <= limits_.max_bytes);
    }

    bool is_reliable = (mode == mailbox::DeliveryMode::AtLeastOnce ||
                        mode == mailbox::DeliveryMode::DurableAtLeastOnce);

    size_t data_limit_messages;
    size_t data_limit_bytes;
    if (is_reliable) {
        data_limit_messages = effective_messages;
        data_limit_bytes = effective_bytes;
    } else {
        data_limit_messages =
            static_cast<size_t>(static_cast<double>(effective_messages) *
                                (1.0 - limits_.reliable_headroom_pct));
        data_limit_bytes =
            static_cast<size_t>(static_cast<double>(effective_bytes) *
                                (1.0 - limits_.reliable_headroom_pct));
    }

    return (data_msgs < data_limit_messages) &&
           (data_bytes + msg_bytes <= data_limit_bytes);
}

mailbox::EnqueueResult
EndpointOutboundQueue::try_enqueue(PendingMessage msg,
                                   mailbox::DeliveryMode mode, TypeTag type_tag) {
    bool is_control = mailbox::is_system_message(type_tag);
    size_t msg_bytes = msg.data.size();

    while (spinlock_.test_and_set(std::memory_order_acquire)) {
    }

    bool admitted = check_admission(msg_bytes, is_control, mode);

    if (!admitted) {
        update_pressure_after_enqueue();
        spinlock_.clear(std::memory_order_release);
        mailbox::EnqueueResult result;
        result.code = mailbox::EnqueueResultCode::EndpointBackpressure;
        result.pressure_ratio = depth_ratio();
        result.pressure_state = pressure_.current_state();
        double rate = drain_rate_ema_.load(std::memory_order_acquire);
        if (rate > 0.0 && msg_bytes > 0) {
            double remaining = static_cast<double>(msg_bytes) / rate;
            result.retry_after = std::chrono::milliseconds(
                static_cast<long long>(remaining * 1000.0));
        }
        return result;
    }

    if (is_control) {
        control_messages_.fetch_add(1, std::memory_order_release);
        control_bytes_.fetch_add(msg_bytes, std::memory_order_release);
        control_lane_.push_back(std::move(msg));
    } else {
        data_messages_.fetch_add(1, std::memory_order_release);
        data_bytes_.fetch_add(msg_bytes, std::memory_order_release);
        data_lane_.push_back(std::move(msg));
    }

    update_pressure_after_enqueue();
    spinlock_.clear(std::memory_order_release);

    return mailbox::EnqueueResult{};
}

std::optional<PendingMessage> EndpointOutboundQueue::try_dequeue() {
    if (!control_lane_.empty()) {
        auto msg = std::move(control_lane_.front());
        control_lane_.pop_front();
        size_t sz = msg.data.size();
        control_messages_.fetch_sub(1, std::memory_order_release);
        control_bytes_.fetch_sub(sz, std::memory_order_release);
        update_pressure_after_dequeue(sz);
        return msg;
    }
    if (!data_lane_.empty()) {
        auto msg = std::move(data_lane_.front());
        data_lane_.pop_front();
        size_t sz = msg.data.size();
        data_messages_.fetch_sub(1, std::memory_order_release);
        data_bytes_.fetch_sub(sz, std::memory_order_release);
        update_pressure_after_dequeue(sz);
        return msg;
    }
    return std::nullopt;
}

EndpointOutboundCounts EndpointOutboundQueue::snapshot() const {
    return {control_messages_.load(std::memory_order_acquire),
            control_bytes_.load(std::memory_order_acquire),
            data_messages_.load(std::memory_order_acquire),
            data_bytes_.load(std::memory_order_acquire)};
}

mailbox::MailboxPressureState EndpointOutboundQueue::pressure_state() const {
    return pressure_.current_state();
}

double EndpointOutboundQueue::depth_ratio() const {
    if (limits_.max_messages == 0)
        return 0.0;
    return static_cast<double>(total_messages()) /
           static_cast<double>(limits_.max_messages);
}

size_t EndpointOutboundQueue::total_messages() const {
    return control_messages_.load(std::memory_order_acquire) +
           data_messages_.load(std::memory_order_acquire);
}

size_t EndpointOutboundQueue::total_bytes() const {
    return control_bytes_.load(std::memory_order_acquire) +
           data_bytes_.load(std::memory_order_acquire);
}

size_t EndpointOutboundQueue::control_messages() const {
    return control_messages_.load(std::memory_order_acquire);
}

size_t EndpointOutboundQueue::data_messages() const {
    return data_messages_.load(std::memory_order_acquire);
}

void EndpointOutboundQueue::update_pressure_after_enqueue() {
    double ratio = depth_ratio();
    bool hard_failure = (ratio >= limits_.critical_watermark);
    pressure_.update(ratio, hard_failure, limits_.high_watermark,
                     limits_.low_watermark, limits_.critical_watermark);
}

void EndpointOutboundQueue::update_pressure_after_dequeue(size_t bytes_dequeued) {
    double ratio = depth_ratio();
    bool hard_failure = (ratio >= limits_.critical_watermark);
    pressure_.update(ratio, hard_failure, limits_.high_watermark,
                     limits_.low_watermark, limits_.critical_watermark);

    double alpha = limits_.drain_rate_ema_alpha;
    double current = drain_rate_ema_.load(std::memory_order_acquire);
    double updated =
        alpha * static_cast<double>(bytes_dequeued) + (1.0 - alpha) * current;
    drain_rate_ema_.store(updated, std::memory_order_release);
}

} // namespace hpactor::net
