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

#include <cstdint>

namespace hpactor::mailbox {

/// \brief Delivery guarantee requested by the sender.
///
/// The mode governs whether the runtime tracks the message after send,
/// retries on transient failure, suppresses duplicates at the receiver,
/// and persists the message for crash recovery.
enum class DeliveryMode : uint8_t {
    /// Fire-and-forget. The runtime attempts delivery once. No delivery
    /// result is returned to the sender. Failure is recorded via
    /// metrics/logging/DLQ when those subsystems are enabled.
    BestEffort = 0,

    /// Single delivery attempt with a result returned to the caller.
    /// The caller decides whether to retry, slow down, or fail its
    /// request. No automatic retry is performed by the runtime.
    ObservableBestEffort = 1,

    /// The sender keeps an outbound record until ACK, timeout, or
    /// cancellation. The receiver may observe duplicates. Messages must
    /// carry a stable MessageId. Receiver-side deduplication is
    /// available when enabled. Caller must ensure handler idempotency.
    AtLeastOnce = 2,

    /// At-least-once delivery with durable outbox/inbox persistence.
    /// The sender persists an outbox record before network send. The
    /// receiver persists an inbox/dedup record before ACK. Recovery
    /// replays unacknowledged messages.
    DurableAtLeastOnce = 3,
};

/// \brief Human-readable snake_case string for metrics labels, log keys,
///        and CLI.
///
/// \param[in] mode The delivery mode.
/// \return A null-terminated snake_case string literal. Never returns
///         nullptr.
constexpr const char* to_string(DeliveryMode mode) noexcept {
    switch (mode) {
        case DeliveryMode::BestEffort:
            return "best_effort";
        case DeliveryMode::ObservableBestEffort:
            return "observable_best_effort";
        case DeliveryMode::AtLeastOnce:
            return "at_least_once";
        case DeliveryMode::DurableAtLeastOnce:
            return "durable_at_least_once";
    }
    return "best_effort";
}

/// \brief Whether a delivery mode implies stronger-than-best-effort
///        tracking (outbox, dedup, or retry state).
///
/// \param[in] mode The delivery mode.
/// \return true if the mode requires runtime delivery tracking beyond a
///         single fire-and-forget attempt.
constexpr bool is_tracked_delivery(DeliveryMode mode) noexcept {
    return mode >= DeliveryMode::AtLeastOnce;
}

} // namespace hpactor::mailbox
