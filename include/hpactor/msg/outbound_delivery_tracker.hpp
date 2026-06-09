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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/delivery_receipt.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/msg/retry_policy.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hpactor {
namespace metrics {
enum class MetricEventType : uint8_t;
}
} // namespace hpactor

namespace hpactor::msg {

/// \brief Tracks in-flight at-least-once sends and drives the retry loop.
///
/// One instance per \c ActorSystem. Maintains a map of
/// \c MessageId → \c PendingSend. Retry timers are driven by
/// \c process_retries(), called periodically from the network loop.
///
/// \note Thread safety: all public methods acquire an internal mutex.
///       Safe to call from transport callbacks and the scheduler tick.
class OutboundDeliveryTracker {
  public:
    /// \brief State for a single in-flight tracked delivery.
    struct PendingSend {
        MessageId msg_id;              ///< Unique message identifier.
        StreamBuffer serialized_frame; ///< Pre-serialized, reused on retry.
        EndPoint remote_endpoint;      ///< Target endpoint for resend.
        RetryPolicy policy;            ///< Retry configuration.
        uint8_t retry_count = 0;       ///< Retries already attempted.
        uint64_t deadline_ns = 0;   ///< Absolute monotonic deadline (0 = none).
        uint64_t next_retry_ns = 0; ///< Absolute monotonic (0 = awaiting ACK).
        DeliveryReceipt receipt;    ///< Handle returned to caller.
    };

    /// \brief Callback for emitting metrics events.
    ///
    /// \param[in] timestamp_ns Current monotonic timestamp.
    /// \param[in] type         Metric event type.
    /// \param[in] code         Event-specific code (DeliveryStatus, etc.).
    using MetricsCallback =
        std::function<void(uint64_t timestamp_ns,
                           ::hpactor::metrics::MetricEventType type, uint8_t code)>;

    OutboundDeliveryTracker();
    ~OutboundDeliveryTracker();

    OutboundDeliveryTracker(const OutboundDeliveryTracker&) = delete;
    OutboundDeliveryTracker& operator=(const OutboundDeliveryTracker&) = delete;

    /// \brief Set the optional metrics callback.
    ///
    /// When set, the tracker emits events for key state transitions
    /// (track, ACK, NACK, retry, exhaustion, cancel).
    void set_metrics_callback(MetricsCallback cb) {
        metrics_cb_ = std::move(cb);
    }

    /// \brief Start tracking a new send.
    ///
    /// The caller has already serialized the frame and sent it once.
    /// The tracker stores the serialized bytes for retry resend.
    ///
    /// \param[in] serialized_frame Pre-serialized wire bytes.
    /// \param[in] remote           Target endpoint for resend.
    /// \param[in] policy           Retry configuration.
    /// \param[in] deadline_ns      Absolute monotonic deadline (0 = none).
    /// \return A \c DeliveryReceipt the caller can observe.
    [[nodiscard]] DeliveryReceipt
    track(StreamBuffer serialized_frame, EndPoint remote, RetryPolicy policy,
          uint64_t deadline_ns);

    /// \brief Called by transport when an AckFrame arrives.
    ///
    /// Resolves the receipt with \c DeliveryStatus::Accepted and removes
    /// the entry from the pending map.
    void on_ack(MessageId msg_id, EndPoint from_endpoint);

    /// \brief Called by transport when a NackFrame arrives.
    ///
    /// Retryable reasons (\c MailboxFull) schedule a retry. Non-retryable
    /// reasons resolve the receipt immediately. \c Duplicate is treated
    /// as an ACK.
    ///
    /// \param[in] msg_id         The message being nack'd.
    /// \param[in] from_endpoint  The endpoint that sent the nack.
    /// \param[in] reason_code    Maps to \c DeliveryStatus.
    /// \param[in] retry_after_ms Sender hint; 0 = sender decides backoff.
    void on_nack(MessageId msg_id, EndPoint from_endpoint, uint32_t reason_code,
                 uint32_t retry_after_ms);

    /// \brief Poll retry timers — called periodically from the network loop.
    ///
    /// For each \c PendingSend whose \c next_retry_ns has elapsed:
    /// - If \c retry_count >= \c max_attempts or deadline expired: resolves
    ///   the receipt with \c TransportError (or \c Expired) and removes the
    ///   entry. Callers should inspect the resolved result and push a
    ///   \c DeadLetterRecord with \c RetryExhausted when appropriate.
    /// - Otherwise: calls \c resend_callback with the \c PendingSend.
    ///
    /// \param[in] now_ns          Current monotonic timestamp.
    /// \param[in] resend_callback Invoked for each entry due for retry.
    void process_retries(uint64_t now_ns,
                         std::function<void(const PendingSend&)> resend_callback);

    /// \brief Cancel all pending sends for a disconnected endpoint.
    ///
    /// \param[in] endpoint The disconnected endpoint.
    /// \param[in] reason   Status to resolve with (typically \c
    /// RemoteUnavailable).
    void cancel_endpoint(EndPoint endpoint, mailbox::DeliveryStatus reason);

    /// \brief Cancel tracking for a specific message.
    ///
    /// Resolves with \c DeliveryStatus::Cancelled.
    void cancel(MessageId msg_id);

    /// \brief Number of currently pending tracked sends.
    [[nodiscard]] size_t pending() const noexcept;

    /// \brief Lightweight snapshot entry for CLI introspection.
    struct SnapshotEntry {
        MessageId msg_id;
        uint8_t retry_count = 0;
        uint64_t deadline_ns = 0;
        uint64_t next_retry_ns = 0;
    };

    /// \brief Snapshot of pending send metadata for CLI introspection.
    [[nodiscard]] std::vector<SnapshotEntry> snapshot() const;

  private:
    void resolve(MessageId msg_id, mailbox::DeliveryResult result);
    void emit_metric(::hpactor::metrics::MetricEventType type, uint8_t code);

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, PendingSend> pending_;
    std::atomic<uint64_t> next_msg_id_{1};
    MetricsCallback metrics_cb_;
};

} // namespace hpactor::msg
