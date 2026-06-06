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

#include <hpactor/msg/delivery_mode.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>

namespace hpactor {

/// \brief Result of a transport-level send attempt.
///
/// Defined here (rather than net/transport.hpp) to avoid a circular
/// include between mailbox/delivery_result.hpp and net/transport.hpp.
/// The canonical definition lives in this header; transport.hpp
/// references it via this include.
enum class TransportSendResult : uint8_t {
    /// Frame was queued for transmission.
    Sent = 0,

    /// No connection to the target endpoint exists.
    NotConnected = 1,

    /// The outbound queue for this endpoint is at capacity.
    QueueFull = 2,

    /// The endpoint circuit breaker is open.
    CircuitOpen = 3,

    /// The frame could not be serialized (encode failed).
    EncodeError = 4,

    /// The transport is shutting down.
    ShuttingDown = 5,

    /// Write to the socket failed (connection lost mid-write).
    WriteError = 6,
};

namespace mailbox {

/// \brief User-facing delivery outcome.
///
/// Flat status enum representing what happened to a message send
/// attempt, regardless of whether it was local or remote.
enum class DeliveryStatus : uint8_t {
    /// Message was accepted for delivery.
    Accepted = 0,

    /// Accepted but the target actor or endpoint is under pressure.
    AcceptedWithPressure = 1,

    /// No route to the target actor or node could be resolved.
    NoRoute = 2,

    /// The target actor is terminated or was never spawned.
    ActorDead = 3,

    /// The target mailbox is at hard capacity.
    MailboxFull = 4,

    /// The message deadline expired before delivery could complete.
    Expired = 5,

    /// A duplicate message was suppressed by the receiver dedup cache.
    Duplicate = 6,

    /// The remote node or endpoint is not reachable.
    RemoteUnavailable = 7,

    /// Rejected by mailbox overflow policy, security policy, or actor
    /// lifecycle gate.
    RejectedByPolicy = 8,

    /// Message serialization failed.
    SerializationError = 9,

    /// Transport-level failure: connection lost, send buffer full,
    /// circuit breaker open, or transport shutting down.
    TransportError = 10,

    /// The local actor system is shutting down and not accepting new
    /// messages.
    ShuttingDown = 11,
};

/// \brief Check if a DeliveryStatus represents successful delivery.
[[nodiscard]] constexpr bool is_accepted(DeliveryStatus s) noexcept {
    return s == DeliveryStatus::Accepted ||
           s == DeliveryStatus::AcceptedWithPressure;
}

/// \brief Check if a DeliveryStatus indicates a retryable failure.
[[nodiscard]] constexpr bool is_retryable(DeliveryStatus s) noexcept {
    switch (s) {
        case DeliveryStatus::NoRoute:
        case DeliveryStatus::ActorDead:
        case DeliveryStatus::MailboxFull:
        case DeliveryStatus::RemoteUnavailable:
        case DeliveryStatus::TransportError:
            return true;
        default:
            return false;
    }
}

/// \brief Map a DeliveryStatus to the canonical FailureReason.
[[nodiscard]] constexpr FailureReason to_failure_reason(DeliveryStatus s) noexcept {
    switch (s) {
        case DeliveryStatus::Accepted:
        case DeliveryStatus::AcceptedWithPressure:
            return FailureReason::Unknown;
        case DeliveryStatus::NoRoute:
            return FailureReason::NoRoute;
        case DeliveryStatus::ActorDead:
            return FailureReason::ActorDead;
        case DeliveryStatus::MailboxFull:
            return FailureReason::MailboxFull;
        case DeliveryStatus::Expired:
            return FailureReason::Expired;
        case DeliveryStatus::Duplicate:
            return FailureReason::Duplicate;
        case DeliveryStatus::RemoteUnavailable:
            return FailureReason::RemoteUnavailable;
        case DeliveryStatus::RejectedByPolicy:
            return FailureReason::RejectedByPolicy;
        case DeliveryStatus::SerializationError:
            return FailureReason::SerializationError;
        case DeliveryStatus::TransportError:
            return FailureReason::TransportError;
        case DeliveryStatus::ShuttingDown:
            return FailureReason::ShuttingDown;
    }
    return FailureReason::Unknown;
}

/// \brief Human-readable snake_case string for metrics, logs, and CLI.
[[nodiscard]] const char* to_string(DeliveryStatus s) noexcept;

/// \brief Unified user-facing delivery outcome.
///
/// Carries the delivery status plus enough metadata for the caller to
/// decide whether to retry, fail, or slow down. Separated from
/// \c EnqueueResult so that user code does not depend on mailbox
/// internals (pressure ratio, depth, retry_after).
struct DeliveryResult {
    DeliveryStatus status{DeliveryStatus::Accepted};
    ActorAddress target;
    MessageId message_id{};
    uint32_t detail_code{0};

    [[nodiscard]] bool ok() const noexcept {
        return is_accepted(status);
    }
    [[nodiscard]] bool accepted() const noexcept {
        return is_accepted(status);
    }
    [[nodiscard]] bool retryable() const noexcept {
        return is_retryable(status);
    }
    [[nodiscard]] FailureReason failure_reason() const noexcept {
        return to_failure_reason(status);
    }

    /// Build from a local mailbox admission result.
    static DeliveryResult
    from_enqueue(const EnqueueResult& er, const ActorAddress& target_addr,
                 MessageId msg_id = {});

    /// Build from a remote transport send result.
    static DeliveryResult
    from_transport(TransportSendResult tsr, const ActorAddress& target_addr,
                   MessageId msg_id = {});
};

} // namespace mailbox
} // namespace hpactor
