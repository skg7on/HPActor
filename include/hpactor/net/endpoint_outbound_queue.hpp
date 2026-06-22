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
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>

#include <atomic>

namespace hpactor::net {

/// \brief Pending message entry for outbound queue storage.
struct PendingMessage {
    /// \brief Destination actor address.
    ActorAddress target;
    /// \brief Serialized message payload.
    StreamBuffer data;
    /// \brief Timestamp when the message was enqueued.
    std::chrono::steady_clock::time_point enqueued_at;
};

/// \brief Capacity and watermarks for an outbound endpoint queue.
struct EndpointOutboundLimits {
    /// \brief Maximum number of queued messages (default 1000).
    size_t max_messages = 1000;
    /// \brief Maximum bytes in the queue (default 16 MiB).
    size_t max_bytes = 16 * 1024 * 1024;
    /// \brief Reserved slots for control messages.
    size_t control_lane_reserve = 64;
    /// \brief Headroom reserved for reliable delivery messages (20%).
    double reliable_headroom_pct = 0.20;
    /// \brief High-pressure watermark (70% capacity).
    double high_watermark = 0.70;
    /// \brief Critical-pressure watermark (90% capacity).
    double critical_watermark = 0.90;
    /// \brief Low-pressure watermark for hysteresis (50% capacity).
    double low_watermark = 0.50;
    /// \brief EMA alpha for drain rate estimation.
    double drain_rate_ema_alpha = 0.20;
};

/// \brief Snapshot of outbound queue counts.
struct EndpointOutboundCounts {
    size_t control_messages = 0;
    size_t control_bytes = 0;
    size_t data_messages = 0;
    size_t data_bytes = 0;
};

/// \brief Bounded outbound queue for a single remote endpoint.
///
/// Maintains separate control and data lanes with admission control,
/// backpressure signaling, and pressure state tracking. Used by
/// \c ConnectionPool to gate outbound message flow before the
/// kernel socket buffer.
///
/// \note Thread safety: \c try_enqueue() and \c try_dequeue() use an
///       atomic spinlock. \c snapshot() and \c pressure_state() read
///       atomics without locking.
class EndpointOutboundQueue {
  public:
    /// \brief Construct with capacity limits.
    ///
    /// \param[in] limits Queue capacity and watermark configuration.
    explicit EndpointOutboundQueue(const EndpointOutboundLimits& limits);

    /// \brief Try to enqueue a message.
    ///
    /// Checks admission control, circuit breaker, and watermarks.
    /// \param[in] msg Message to enqueue.
    /// \param[in] mode Delivery mode (affects admission).
    /// \param[in] type_tag Type tag for control/data classification.
    /// \return Result describing whether the message was queued or why
    ///         it was rejected.
    TransportSendResult
    try_enqueue(PendingMessage msg, mailbox::DeliveryMode mode, TypeTag type_tag);

    /// \brief Try to dequeue the next message (priority: control lane first).
    ///
    /// \return The dequeued message, or \c std::nullopt if both lanes
    ///         are empty.
    std::optional<PendingMessage> try_dequeue();

    /// \brief Snapshot of current queue counts.
    ///
    /// \return Counts for both control and data lanes.
    EndpointOutboundCounts snapshot() const;

    /// \brief Current backpressure state.
    ///
    /// \return \c Low, \c High, or \c Critical.
    mailbox::MailboxPressureState pressure_state() const;

    /// \brief Current queue depth as a fraction of max capacity.
    ///
    /// \return Ratio in [0.0, 1.0] (may exceed 1.0 under burst).
    double depth_ratio() const;

    /// \brief Total number of messages across both lanes.
    size_t total_messages() const;
    /// \brief Total bytes across both lanes.
    size_t total_bytes() const;
    /// \brief Number of control messages.
    size_t control_messages() const;
    /// \brief Number of data messages.
    size_t data_messages() const;

  private:
    bool check_admission(size_t msg_bytes, bool is_control,
                         mailbox::DeliveryMode mode) const;
    void update_pressure_after_enqueue();
    void update_pressure_after_dequeue(size_t bytes_dequeued);

    EndpointOutboundLimits limits_;
    std::atomic<size_t> control_messages_{0};
    std::atomic<size_t> control_bytes_{0};
    std::atomic<size_t> data_messages_{0};
    std::atomic<size_t> data_bytes_{0};
    std::deque<PendingMessage> control_lane_;
    std::deque<PendingMessage> data_lane_;
    mailbox::detail::PressureStateMachine pressure_;
    std::atomic<double> drain_rate_ema_{0.0};
    std::atomic_flag spinlock_ = ATOMIC_FLAG_INIT;
};

} // namespace hpactor::net
