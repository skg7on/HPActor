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
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/failure_envelope.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace hpactor::mailbox {

/// \brief Reason a message was routed to the dead-letter queue.
///
/// Each value describes the failure that prevented delivery. The numeric
/// encoding is stable for wire compatibility — new values must be appended.
enum class DeadLetterReason : uint8_t {
    MailboxFull,     ///< Target mailbox was at hard capacity.
    MailboxClosed,   ///< Target mailbox was closed (actor draining or stopped).
    ActorNotFound,   ///< Target actor was not found in the registry.
    ActorTerminated, ///< Target actor was already terminated.
    MissingRoute,    ///< No route to the target actor or node.
    RemoteNodeUnreachable, ///< Remote node is not reachable.
    NetworkPartition,      ///< Network partition detected.
    TransportSendFailed,   ///< Transport-level send failure (write error,
                           ///< connection lost).
    DecodeFailed,          ///< Message decoding/deserialization failed.
    OverflowPolicy,        ///< Mailbox overflow policy sent the message to DLQ.
    NoDropRejected,    ///< No-drop flag was set and the message was rejected.
    DrainTimeout = 12, ///< Message dropped because drain deadline expired.
    DrainPolicyDrop = 13, ///< Message dropped by DropUserMessages drain policy.
    Expired = 14,         ///< Message deadline expired before delivery.
    EndpointBackpressure = 15, ///< Data lane at capacity for the target
                               ///< endpoint.
    EndpointCircuitOpen = 16, ///< Circuit breaker open for the target endpoint.
    AskTimeout = 17,          ///< Ask/request timed out without a response.
};

/// \brief Origin of a dead-lettered message within the system.
enum class DeadLetterSource : uint8_t {
    LocalDelivery,    ///< Local actor-to-actor delivery path.
    RemoteDelivery,   ///< Remote delivery via transport.
    ActorProxy,       ///< ActorProxy send path.
    Transport,        ///< Transport layer (encode/send failure).
    MailboxAdmission, ///< Mailbox admission gate (policy, capacity).
    ServiceDiscovery, ///< Service discovery failure (no route, node gone).
    Replay,           ///< DLQ replay attempt.
};

/// \brief Map a \c DeadLetterReason to the canonical \c FailureReason.
///
/// \param[in] reason The dead-letter reason to map.
/// \return The corresponding \c FailureReason enum value.
/// \note Thread safety: constexpr — safe to call from any context.
[[nodiscard]] constexpr FailureReason
failure_reason(DeadLetterReason reason) noexcept {
    switch (reason) {
        case DeadLetterReason::MailboxFull:
            return FailureReason::MailboxFull;
        case DeadLetterReason::MailboxClosed:
            return FailureReason::MailboxClosed;
        case DeadLetterReason::ActorNotFound:
            return FailureReason::NoRoute;
        case DeadLetterReason::ActorTerminated:
            return FailureReason::ActorDead;
        case DeadLetterReason::MissingRoute:
            return FailureReason::NoRoute;
        case DeadLetterReason::RemoteNodeUnreachable:
            return FailureReason::NodeUnavailable;
        case DeadLetterReason::NetworkPartition:
            return FailureReason::NodeUnavailable;
        case DeadLetterReason::TransportSendFailed:
            return FailureReason::TransportError;
        case DeadLetterReason::DecodeFailed:
            return FailureReason::SerializationError;
        case DeadLetterReason::OverflowPolicy:
            return FailureReason::RejectedByPolicy;
        case DeadLetterReason::NoDropRejected:
            return FailureReason::RejectedByPolicy;
        case DeadLetterReason::DrainTimeout:
            return FailureReason::Timeout;
        case DeadLetterReason::DrainPolicyDrop:
            return FailureReason::Dropped;
        case DeadLetterReason::Expired:
            return FailureReason::Expired;
        case DeadLetterReason::AskTimeout:
            return FailureReason::Timeout;
        case DeadLetterReason::EndpointBackpressure:
            return FailureReason::ResourceExhausted;
        case DeadLetterReason::EndpointCircuitOpen:
            return FailureReason::RemoteUnavailable;
    }
    return FailureReason::Unknown;
}

/// \brief Map a \c DeadLetterSource to the canonical \c FailureSource.
///
/// \param[in] source The dead-letter source to map.
/// \return The corresponding \c FailureSource enum value.
/// \note Thread safety: constexpr — safe to call from any context.
[[nodiscard]] constexpr FailureSource
failure_source(DeadLetterSource source) noexcept {
    switch (source) {
        case DeadLetterSource::LocalDelivery:
            return FailureSource::ActorRuntime;
        case DeadLetterSource::RemoteDelivery:
            return FailureSource::Transport;
        case DeadLetterSource::ActorProxy:
            return FailureSource::ActorRuntime;
        case DeadLetterSource::Transport:
            return FailureSource::Transport;
        case DeadLetterSource::MailboxAdmission:
            return FailureSource::Mailbox;
        case DeadLetterSource::ServiceDiscovery:
            return FailureSource::Discovery;
        case DeadLetterSource::Replay:
            return FailureSource::ActorRuntime;
    }
    return FailureSource::Unknown;
}

/// \brief Human-readable string for a \c DeadLetterReason.
///
/// \param[in] reason The dead-letter reason.
/// \return A null-terminated string literal. Never returns \c nullptr.
[[nodiscard]] inline const char* to_string(DeadLetterReason reason) noexcept {
    switch (reason) {
        case DeadLetterReason::MailboxFull:
            return "MailboxFull";
        case DeadLetterReason::MailboxClosed:
            return "MailboxClosed";
        case DeadLetterReason::ActorNotFound:
            return "ActorNotFound";
        case DeadLetterReason::ActorTerminated:
            return "ActorTerminated";
        case DeadLetterReason::MissingRoute:
            return "MissingRoute";
        case DeadLetterReason::RemoteNodeUnreachable:
            return "RemoteNodeUnreachable";
        case DeadLetterReason::NetworkPartition:
            return "NetworkPartition";
        case DeadLetterReason::TransportSendFailed:
            return "TransportSendFailed";
        case DeadLetterReason::DecodeFailed:
            return "DecodeFailed";
        case DeadLetterReason::OverflowPolicy:
            return "OverflowPolicy";
        case DeadLetterReason::NoDropRejected:
            return "NoDropRejected";
        case DeadLetterReason::DrainTimeout:
            return "DrainTimeout";
        case DeadLetterReason::DrainPolicyDrop:
            return "DrainPolicyDrop";
        case DeadLetterReason::Expired:
            return "Expired";
        case DeadLetterReason::AskTimeout:
            return "ask_timeout";
        case DeadLetterReason::EndpointBackpressure:
            return "EndpointBackpressure";
        case DeadLetterReason::EndpointCircuitOpen:
            return "EndpointCircuitOpen";
    }
    return "Unknown";
}

/// \brief Human-readable string for a \c DeadLetterSource.
///
/// \param[in] source The dead-letter source.
/// \return A null-terminated string literal. Never returns \c nullptr.
[[nodiscard]] inline const char* to_string(DeadLetterSource source) noexcept {
    switch (source) {
        case DeadLetterSource::LocalDelivery:
            return "LocalDelivery";
        case DeadLetterSource::RemoteDelivery:
            return "RemoteDelivery";
        case DeadLetterSource::ActorProxy:
            return "ActorProxy";
        case DeadLetterSource::Transport:
            return "Transport";
        case DeadLetterSource::MailboxAdmission:
            return "MailboxAdmission";
        case DeadLetterSource::ServiceDiscovery:
            return "ServiceDiscovery";
        case DeadLetterSource::Replay:
            return "Replay";
    }
    return "Unknown";
}

/// \brief Eviction policy when the dead-letter queue reaches capacity.
enum class DeadLetterOverflowPolicy : uint8_t {
    DropOldestRecord, ///< Evict the oldest record to make room.
    DropNewestRecord, ///< Drop the incoming record.
    MetadataOnly,     ///< Store only metadata, discard the payload sample.
};

/// \brief Configuration for a \c DeadLetterQueue instance.
struct DeadLetterConfig {
    bool enabled = true;
    uint32_t capacity = 4096;
    uint64_t byte_capacity = 0;
    uint32_t max_payload_sample_bytes = 512;
    DeadLetterOverflowPolicy overflow_policy =
        DeadLetterOverflowPolicy::DropOldestRecord;
    bool store_payload = true;
    bool alert_on_first_failure = false;
    uint32_t alert_threshold_per_minute = 100;
};

/// \brief A single dead-letter record stored in the dead-letter queue.
///
/// Captures the full context of a failed delivery: the reason, source,
/// sender/target addresses, message metadata, trace context, a truncated
/// payload sample, and mailbox state at the time of failure.
struct DeadLetterRecord {
    DeadLetterReason reason = DeadLetterReason::ActorNotFound; ///< Why delivery
                                                               ///< failed.
    DeadLetterSource source = DeadLetterSource::LocalDelivery; ///< Where in the
                                                               ///< system the
                                                               ///< failure
                                                               ///< originated.
    ActorAddress sender;                 ///< Address of the sending actor.
    ActorAddress target;                 ///< Address of the intended recipient.
    TypeTag type_tag = TypeTag::Invalid; ///< Message type tag for
                                         ///< classification.
    uint64_t message_id = 0;         ///< Message identifier for correlation.
    uint32_t frame_flags = 0;        ///< Transport frame flags at send time.
    uint8_t priority = 0;            ///< Message priority level.
    int64_t deadline_ns = INT64_MAX; ///< Delivery deadline in nanoseconds
                                     ///< (INT64_MAX = none).
    uint64_t trace_id_hi = 0;        ///< W3C trace ID high 64 bits.
    uint64_t trace_id_lo = 0;        ///< W3C trace ID low 64 bits.
    uint64_t span_id = 0;            ///< W3C span ID.
    uint32_t payload_size = 0;       ///< Full payload size before truncation.
    StreamBuffer payload_sample;   ///< Truncated payload sample for inspection.
    uint32_t mailbox_depth = 0;    ///< Mailbox depth at failure time.
    uint32_t mailbox_capacity = 0; ///< Mailbox capacity at failure time.
    uint64_t timestamp_ns = 0;     ///< Monotonic timestamp when the record was
                                   ///< created.

    /// Build a FailureEnvelope from this dead-letter record's fields.
    [[nodiscard]] FailureEnvelope to_failure_envelope() const noexcept {
        FailureEnvelope env;
        env.reason = failure_reason(reason);
        env.actor_id = target.id;
        env.sender = sender;
        env.receiver = target;
        env.message_id = MessageId{message_id};
        env.trace = TraceContext{}; // DLQ records don't yet carry full trace
                                    // context
        env.retryable = ::hpactor::retryable(env.reason);
        env.timestamp_ns = timestamp_ns;
        env.source = failure_source(source);
        return env;
    }
};

/// \brief Point-in-time snapshot of dead-letter queue counters.
///
/// Captured atomically under the queue's internal mutex for observability.
struct DeadLetterQueueSnapshot {
    uint32_t depth = 0;        ///< Current number of stored records.
    uint32_t capacity = 0;     ///< Configured maximum record count.
    uint64_t total_pushed = 0; ///< Cumulative records ever pushed.
    uint64_t total_popped = 0; ///< Cumulative records ever popped (replayed or
                               ///< removed).
    uint64_t total_lost = 0;   ///< Cumulative records evicted due to overflow.
};

/// \brief Sink interface for dead-letter record notifications.
///
/// Implementations receive a callback for every record pushed into the DLQ,
/// enabling real-time alerting, external logging, or metrics export.
///
/// \note Thread safety: \c on_dead_letter() is called under the DLQ's
///       internal mutex. Implementations must not block for long periods.
class IDeadLetterSink {
  public:
    virtual ~IDeadLetterSink() = default;

    /// \brief Called when a new dead-letter record is pushed.
    ///
    /// \param[in] record The dead-letter record (by const reference, valid
    ///                   for the duration of the call).
    /// \note Called under the DLQ's internal mutex — keep it fast.
    virtual void on_dead_letter(const DeadLetterRecord& record) noexcept = 0;
};

/// \brief Bounded dead-letter queue for failed message delivery records.
///
/// Stores records of messages that could not be delivered, with configurable
/// capacity, payload sampling, overflow policy, and optional sink callbacks.
/// Supports replay (re-deliver a stored record), export, and per-actor
/// filtering via \c snapshot_records().
///
/// \note Thread safety: all public methods are internally synchronized via
///       a \c std::mutex. Safe to call from any thread.
class DeadLetterQueue {
  public:
    /// \brief Construct a dead-letter queue with the given configuration.
    ///
    /// \param[in] config Initial configuration. Defaults to 4096 records,
    ///                   \c DropOldestRecord overflow, payload storage enabled.
    explicit DeadLetterQueue(DeadLetterConfig config = {});

    /// \brief Push a dead-letter record into the queue.
    ///
    /// On overflow, the configured \c DeadLetterOverflowPolicy determines
    /// eviction behavior. The payload sample is trimmed according to
    /// \c max_payload_sample_bytes.
    ///
    /// \param[in,out] record The record to push. The payload sample field may
    ///                       be trimmed by \c trim_payload().
    /// \retval true Record was stored.
    /// \retval false Record was rejected (overflow policy = DropNewestRecord
    ///         and the queue was at capacity).
    /// \note Thread safety: acquires internal mutex.
    bool try_push(DeadLetterRecord&& record) noexcept;

    /// \brief Pop the oldest record from the queue (FIFO).
    ///
    /// \param[out] out Receives the popped record on success; untouched on
    ///                  failure.
    /// \retval true A record was popped and placed in \p out.
    /// \retval false The queue is empty.
    /// \note Thread safety: acquires internal mutex.
    bool try_pop(DeadLetterRecord& out) noexcept;

    /// \brief Read-only access to the queue configuration.
    ///
    /// \return A const reference to the current \c DeadLetterConfig.
    /// \note Thread safety: the returned reference may be invalidated by a
    ///       concurrent mutation. Read from the actor's own thread or during
    ///       a quiescent period.
    const DeadLetterConfig& config() const noexcept {
        return config_;
    }

    /// \brief Capture a point-in-time snapshot of all stored records.
    ///
    /// Copies every record in the queue under the internal mutex. The returned
    /// vector is independent of the queue and safe to inspect without holding
    /// the lock.
    ///
    /// \return A vector containing copies of all stored records.
    /// \note Thread safety: acquires internal mutex. The snapshot is
    ///       atomically consistent at the time of the call.
    std::vector<DeadLetterRecord> snapshot_records() const;

    /// \brief Pop a record at a specific index.
    ///
    /// Used by CLI replay to remove a specific record after successful
    /// re-delivery, without disturbing the FIFO ordering of other records.
    ///
    /// \param[in] index Zero-based index into the internal deque.
    /// \param[out] out Receives the removed record on success.
    /// \retval true Record at \p index was removed.
    /// \retval false \p index is out of range.
    /// \note Thread safety: acquires internal mutex.
    bool try_pop_at(size_t index, DeadLetterRecord& out) noexcept;

    /// \brief Capture a point-in-time summary of queue counters.
    ///
    /// Reads depth, capacity, and cumulative counters atomically under the
    /// internal mutex.
    ///
    /// \return A \c DeadLetterQueueSnapshot with current statistics.
    /// \note Thread safety: acquires internal mutex.
    DeadLetterQueueSnapshot snapshot() const noexcept;

  private:
    void trim_payload(DeadLetterRecord& record) const;

    DeadLetterConfig config_;
    mutable std::mutex mutex_;
    std::deque<DeadLetterRecord> records_;
    uint64_t total_pushed_{0};
    uint64_t total_popped_{0};
    uint64_t total_lost_{0};
};

} // namespace hpactor::mailbox
