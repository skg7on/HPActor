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

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>

namespace hpactor::mailbox {

struct MailboxCapacity {
    uint32_t max_messages = 1024;
    uint64_t max_bytes = 0;
};

enum class OverflowPolicy : uint8_t {
    RejectNewest,
    DropNewest,
    DropOldest,
    DropLowestPriority,
    DeadLetter,
    SpillToOverflowQueue,
    SignalOnly,
    BlockWhenAllowed,
};

enum class BackpressureMode : uint8_t {
    Disabled,
    LocalSignal,
    RemoteSignal,
    LocalAndRemoteSignal,
};

enum class MailboxPressureState : uint8_t {
    Normal,
    SoftPressure,
    HardPressure,
    Recovering,
};

struct MailboxConfig {
    MailboxCapacity capacity;
    uint8_t priority_levels = 4;
    OverflowPolicy overflow_policy = OverflowPolicy::RejectNewest;
    BackpressureMode backpressure_mode = BackpressureMode::LocalAndRemoteSignal;
    double high_watermark = 0.80;
    double low_watermark = 0.50;
    uint32_t protected_system_messages = 32;
    uint32_t max_overflow_depth = 0;
    uint32_t signal_min_interval_ms = 100;
    bool priority_aware = false;
    bool enable_dead_letters = true;
};

struct MessagePriority {
    uint8_t value = 0;
};

struct DeliveryOptions {
    bool no_drop = false;
    bool allow_blocking = false;
    bool emit_backpressure = true;
    uint64_t message_id = 0;
    uint32_t flags = 0;
};

struct MailboxEnvelopeMeta {
    ActorAddress sender;
    TypeTag type_tag = TypeTag::Invalid;
    uint64_t message_id = 0;
    uint8_t priority = 0;
    int64_t deadline_ns = INT64_MAX;
    uint32_t flags = 0;
    uint64_t estimated_bytes = 0;
    uint64_t sequence = 0;
};

enum class EnqueueResultCode : uint8_t {
    Accepted,
    AcceptedWithSoftPressure,
    Rejected,
    DroppedNewest,
    DroppedExisting,
    ReroutedToDeadLetter,
    ReroutedToOverflow,
    MailboxClosed,
    ActorNotFound,
};

/// Map an EnqueueResultCode to the canonical FailureReason.
/// Accepted and AcceptedWithSoftPressure are not failures — callers
/// should guard with !result.accepted() before calling.
[[nodiscard]] constexpr FailureReason
failure_reason(EnqueueResultCode code) noexcept {
    switch (code) {
        case EnqueueResultCode::Rejected:
            return FailureReason::RejectedByPolicy;
        case EnqueueResultCode::DroppedNewest:
        case EnqueueResultCode::DroppedExisting:
            return FailureReason::Dropped;
        case EnqueueResultCode::ReroutedToDeadLetter:
            return FailureReason::RejectedByPolicy;
        case EnqueueResultCode::ReroutedToOverflow:
            return FailureReason::RejectedByPolicy;
        case EnqueueResultCode::MailboxClosed:
            return FailureReason::MailboxClosed;
        case EnqueueResultCode::ActorNotFound:
            return FailureReason::NoRoute;
        case EnqueueResultCode::Accepted:
        case EnqueueResultCode::AcceptedWithSoftPressure:
            return FailureReason::Unknown; // Not a failure
    }
    return FailureReason::Unknown;
}

struct EnqueueResult {
    EnqueueResultCode code = EnqueueResultCode::Accepted;
    ActorId target;
    uint32_t depth = 0;
    uint32_t capacity = 0;
    double pressure_ratio = 0.0;
    std::chrono::milliseconds retry_after{0};
    TypeTag affected_type = TypeTag::Invalid;
    uint64_t affected_message_id = 0;

    [[nodiscard]] EnqueueResultCode status() const noexcept {
        return code;
    }
    [[nodiscard]] bool ok() const noexcept {
        return accepted();
    }

    [[nodiscard]] bool accepted() const noexcept {
        return code == EnqueueResultCode::Accepted ||
               code == EnqueueResultCode::AcceptedWithSoftPressure;
    }

    [[nodiscard]] bool retryable() const noexcept {
        return code == EnqueueResultCode::Rejected ||
               code == EnqueueResultCode::MailboxClosed ||
               code == EnqueueResultCode::ReroutedToOverflow;
    }

    /// Canonical failure reason for this result.
    /// Returns Unknown when the enqueue was accepted.
    [[nodiscard]] FailureReason failure_reason() const noexcept {
        return mailbox::failure_reason(code);
    }
};

enum class BackpressureReason : uint8_t {
    HighWatermark,
    HardCapacity,
    ByteCapacity,
    OverflowPolicy,
    NodeMemoryPressure,
};

struct BackpressureSignal {
    ActorAddress target;
    ActorAddress sender;
    BackpressureReason reason = BackpressureReason::HighWatermark;
    uint32_t depth = 0;
    uint32_t capacity = 0;
    uint64_t bytes = 0;
    uint64_t byte_capacity = 0;
    double pressure_ratio = 0.0;
    std::chrono::milliseconds retry_after{0};
    uint64_t sequence = 0;
};

[[nodiscard]] constexpr bool is_system_message(TypeTag tag) noexcept {
    return static_cast<uint32_t>(tag) > 0 &&
           static_cast<uint32_t>(tag) < static_cast<uint32_t>(TypeTag::User);
}

[[nodiscard]] inline uint64_t
estimate_message_bytes(const TypedMessage& msg) noexcept {
    return sizeof(TypedMessage) + msg.payload().size();
}

inline const char* to_string(OverflowPolicy policy) noexcept {
    switch (policy) {
        case OverflowPolicy::RejectNewest:
            return "reject_newest";
        case OverflowPolicy::DropNewest:
            return "drop_newest";
        case OverflowPolicy::DropOldest:
            return "drop_oldest";
        case OverflowPolicy::DropLowestPriority:
            return "drop_lowest_priority";
        case OverflowPolicy::DeadLetter:
            return "dead_letter";
        case OverflowPolicy::SpillToOverflowQueue:
            return "spill_to_overflow_queue";
        case OverflowPolicy::SignalOnly:
            return "signal_only";
        case OverflowPolicy::BlockWhenAllowed:
            return "block_when_allowed";
    }
    return "reject_newest";
}

inline const char* to_string(MailboxPressureState state) noexcept {
    switch (state) {
        case MailboxPressureState::Normal:
            return "normal";
        case MailboxPressureState::SoftPressure:
            return "soft_pressure";
        case MailboxPressureState::HardPressure:
            return "hard_pressure";
        case MailboxPressureState::Recovering:
            return "recovering";
    }
    return "normal";
}

} // namespace hpactor::mailbox
