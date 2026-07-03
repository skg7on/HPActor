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
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/msg/retry_policy.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace hpactor::mailbox {

// Forward declaration — defined in delivery_result.hpp.
// Cannot include that header here because delivery_result.hpp
// already includes this file.
struct DeliveryResult;

/// \brief Capacity limits for a bounded mailbox.
struct MailboxCapacity {
    uint32_t max_messages = 1024; ///< Hard limit on user-message count (0 =
                                  ///< unbounded, clamped to 1024).
    uint64_t max_bytes = 0; ///< Hard limit on total user-message bytes (0 =
                            ///< unbounded).
};

/// \brief Policy for handling messages when the mailbox is at capacity.
enum class OverflowPolicy : uint8_t {
    RejectNewest, ///< Reject the incoming message with backpressure.
    DropNewest,   ///< Silently drop the incoming message.
    DropOldest,   ///< Evict the oldest message to make room for the new one.
    DropLowestPriority, ///< Evict the lowest-priority message to make room.
    DeadLetter, ///< Route the incoming message to the dead-letter queue.
    SpillToOverflowQueue, ///< Spill to the overflow queue; drain back on
                          ///< dequeue.
    SignalOnly, ///< Emit a backpressure signal without dropping or enqueuing.
    BlockWhenAllowed, ///< Block the producer (only valid for blocking-actor
                      ///< paths).
};

/// \brief Backpressure signal emission mode.
enum class BackpressureMode : uint8_t {
    Disabled,     ///< No backpressure signals are emitted.
    LocalSignal,  ///< Emit backpressure signals to local producers only.
    RemoteSignal, ///< Emit backpressure signals to remote producers only.
    LocalAndRemoteSignal, ///< Emit backpressure signals to both local and
                          ///< remote producers.
};

/// \brief Mailbox pressure state with hysteresis for backpressure control.
///
/// Transitions are governed by \c PressureStateMachine using high/low/critical
/// watermark thresholds to prevent oscillation.
enum class MailboxPressureState : uint8_t {
    Normal,       ///< Depth is below the low watermark.
    SoftPressure, ///< Depth is at or above the high watermark.
    HardPressure, ///< Depth is at or above the critical watermark, or a hard
                  ///< failure occurred.
    Recovering,   ///< Pressure is declining but still above the low watermark.
};

/// \brief Mailbox backend discriminator for per-actor and system-wide defaults.
enum class MailboxBackend : uint8_t {
    VariableMpsc = 0, ///< Variable MPSCActorMailbox (current default).
    Disruptor = 1,    ///< Fixed-message DisruptorActorMailboxCore.
};

/// \brief Full mailbox configuration.
///
/// Controls capacity, watermarks, overflow behavior, backpressure signaling,
/// priority-aware routing, system-message protection, and backend selection.
struct MailboxConfig {
    MailboxCapacity capacity;    ///< Hard limits on message count and bytes.
    uint8_t priority_levels = 4; ///< Number of user priority lanes (1–8).
    OverflowPolicy overflow_policy = OverflowPolicy::RejectNewest; ///< Policy
                                                                   ///< for
                                                                   ///< at-capacity
                                                                   ///< messages.
    BackpressureMode backpressure_mode =
        BackpressureMode::LocalAndRemoteSignal; ///< Backpressure signal
                                                ///< emission mode.
    double high_watermark = 0.80;     ///< Ratio above which \c SoftPressure is
                                      ///< entered.
    double low_watermark = 0.50;      ///< Ratio below which pressure is cleared
                                      ///< (hysteresis).
    double critical_watermark = 1.00; ///< Ratio at or above which \c
                                      ///< HardPressure is entered.
    uint32_t protected_system_messages = 32; ///< Max system-lane depth before
                                             ///< rejection.
    uint32_t max_overflow_depth = 0;       ///< Overflow queue depth limit (0 =
                                           ///< unlimited).
    uint32_t signal_min_interval_ms = 100; ///< Minimum interval between
                                           ///< backpressure signals.
    bool priority_aware = false; ///< Route user messages to per-priority lanes.

    /// Preferred mailbox backend for new actors.
    /// Set \c HPACTOR_DEFAULT_MAILBOX_BACKEND_IS_DISRUPTOR to flip the default.
    MailboxBackend default_backend{
#if HPACTOR_DEFAULT_MAILBOX_BACKEND_IS_DISRUPTOR
        MailboxBackend::Disruptor
#else
        MailboxBackend::VariableMpsc
#endif
    };
};

/// \brief Message priority level for lane routing.
///
/// Lower values indicate higher priority. Priority 0 messages are routed to
/// the highest-priority user lane.
struct MessagePriority {
    uint8_t value = 0; ///< Priority level (0 = highest, up to \c
                       ///< priority_levels - 1).
};

/// \brief Per-message delivery options set by the sender.
struct DeliveryOptions {
    bool no_drop = false;        ///< If true, the message must not be silently
                                 ///< dropped.
    bool allow_blocking = false; ///< If true, the producer may block when the
                                 ///< mailbox is full.
    bool emit_backpressure = true; ///< If true, backpressure signals are
                                   ///< emitted on rejection.
    DeliveryMode delivery_mode = DeliveryMode::BestEffort; ///< Delivery
                                                           ///< guarantee level.
    uint64_t message_id = 0; ///< Sender-assigned message identifier for
                             ///< dedup/correlation.
    uint32_t flags = 0;      ///< Per-message flags (reserved for future use).

    ///< Retry policy for \c AtLeastOnce and \c DurableAtLeastOnce modes.
    ///< When absent, the system-default policy is used (max 5 attempts,
    ///< 5s timeout, exponential backoff 100ms–30s with jitter).
    std::optional<msg::RetryPolicy> retry_policy;

    ///< If true, the work item is placed in the worker\'s EDFQueue instead
    ///< of the priority ChaseLev deque. Requires deadline_ns != INT64_MAX.
    ///< Opt-in, default off — ordinary messages stay on priority queues.
    bool schedule_edf = false;
};

/// \brief Metadata attached to every message enqueued into a mailbox.
///
/// Populated by the producer path before \c try_push(). Used by the mailbox
/// for lane routing, byte accounting, deadline enforcement, and metrics.
struct MailboxEnvelopeMeta {
    ActorAddress sender;                 ///< Address of the sending actor.
    TypeTag type_tag = TypeTag::Invalid; ///< Message type tag for system/user
                                         ///< classification.
    uint64_t message_id = 0; ///< Message identifier for dedup and correlation.
    uint8_t priority = 0;    ///< User priority level (0 = highest).
    int64_t deadline_ns = INT64_MAX; ///< Absolute delivery deadline (INT64_MAX
                                     ///< = none).
    uint32_t flags = 0;              ///< Envelope-level flags.
    uint64_t estimated_bytes = 0; ///< Estimated byte footprint for reservation
                                  ///< accounting.
    uint64_t sequence = 0;     ///< Monotonically increasing sequence number for
                               ///< ordering.
    bool schedule_edf = false; ///< If true, the scheduler places this work
                               ///< item in the EDF queue instead of the
                               ///< priority ChaseLev deque.
};

/// \brief Reason a backpressure signal was emitted.
enum class BackpressureReason : uint8_t {
    HighWatermark,      ///< Mailbox depth crossed the high watermark.
    HardCapacity,       ///< Mailbox is at hard message-count capacity.
    ByteCapacity,       ///< Mailbox is at hard byte capacity.
    OverflowPolicy,     ///< Overflow policy rejected or dropped the message.
    NodeMemoryPressure, ///< Node-level memory pressure triggered backpressure.
};

/// \brief Result code for a mailbox \c try_push() admission attempt.
enum class EnqueueResultCode : uint8_t {
    Accepted,                 ///< Message was enqueued under normal pressure.
    AcceptedWithSoftPressure, ///< Message was enqueued but the mailbox is under
                              ///< pressure.
    Rejected, ///< Message was rejected (policy, capacity, or admission gate).
    DroppedNewest,   ///< Incoming message was dropped by \c DropNewest policy.
    DroppedExisting, ///< An older message was evicted to make room.
    ReroutedToDeadLetter, ///< Message was sent to the dead-letter queue.
    ReroutedToOverflow,   ///< Message was spilled to the overflow queue.
    MailboxClosed,        ///< Mailbox is closed (actor is draining or stopped).
    ActorNotFound,        ///< Target actor was not found.
    EndpointBackpressure, ///< Data lane at capacity for the remote endpoint.
    EndpointCircuitOpen,  ///< Circuit breaker open for the remote endpoint.
    CircuitOpen,          ///< Actor-level circuit breaker is open.
};

/// \brief Map an EnqueueResultCode to the canonical FailureReason.
///
/// \param[in] code The mailbox admission result code.
/// \return The corresponding FailureReason. Returns
///         \c FailureReason::Unknown for \c Accepted and
///         \c AcceptedWithSoftPressure — callers should guard with
///         \c !result.accepted() before calling.
/// \note Thread safety: constexpr and lock-free — safe to call from any
///       thread without synchronization.
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
        case EnqueueResultCode::EndpointBackpressure:
            return FailureReason::ResourceExhausted;
        case EnqueueResultCode::EndpointCircuitOpen:
            return FailureReason::RemoteUnavailable;
        case EnqueueResultCode::CircuitOpen:
            return FailureReason::CircuitOpen;
        case EnqueueResultCode::Accepted:
        case EnqueueResultCode::AcceptedWithSoftPressure:
            return FailureReason::Unknown; // Not a failure
    }
    return FailureReason::Unknown;
}

/// \brief Result of a mailbox \c try_push() admission attempt.
///
/// Carries the admission decision plus detailed mailbox state for the caller
/// to decide whether to retry, back off, or fail. Returned by value — no
/// heap allocation or ownership transfer.
struct EnqueueResult {
    EnqueueResultCode code = EnqueueResultCode::Accepted; ///< Admission result
                                                          ///< code.
    ActorId target;        ///< Target actor identifier.
    uint32_t depth = 0;    ///< Current total mailbox depth after the attempt.
    uint32_t capacity = 0; ///< Configured message-count capacity.
    uint64_t bytes = 0;    ///< Current queued byte count.
    uint64_t byte_capacity = 0; ///< Configured byte capacity (0 = unbounded).
    BackpressureReason pressure_reason =
        BackpressureReason::HighWatermark; ///< Why backpressure was triggered.
    MailboxPressureState pressure_state =
        MailboxPressureState::Normal; ///< Current
                                      ///< pressure
                                      ///< state.
    double pressure_ratio = 0.0;      ///< Current pressure ratio in [0, ∞).
    std::chrono::milliseconds retry_after{0}; ///< Suggested backoff before
                                              ///< retry (0 = retry
                                              ///< immediately).
    TypeTag affected_type = TypeTag::Invalid; ///< Type tag of the affected
                                              ///< message (for
                                              ///< DroppedExisting).
    uint64_t affected_message_id = 0; ///< Message ID of the affected message
                                      ///< (for DroppedExisting).

    [[nodiscard]] EnqueueResultCode status() const noexcept {
        return code;
    }
    [[nodiscard]] bool ok() const noexcept {
        return accepted();
    }

    [[nodiscard]] bool accepted() const noexcept {
        return code == EnqueueResultCode::Accepted ||
               code == EnqueueResultCode::AcceptedWithSoftPressure ||
               code == EnqueueResultCode::ReroutedToOverflow;
    }

    [[nodiscard]] bool retryable() const noexcept {
        return code == EnqueueResultCode::Rejected ||
               code == EnqueueResultCode::MailboxClosed ||
               code == EnqueueResultCode::ReroutedToOverflow ||
               code == EnqueueResultCode::EndpointBackpressure ||
               code == EnqueueResultCode::CircuitOpen;
    }

    /// \brief Canonical failure reason for this admission result.
    ///
    /// \return The FailureReason corresponding to the \c code field.
    ///         Returns \c FailureReason::Unknown when the enqueue was
    ///         accepted.
    [[nodiscard]] FailureReason failure_reason() const noexcept {
        return mailbox::failure_reason(code);
    }

    /// \brief Convert to the user-facing DeliveryResult type.
    ///
    /// \param[in] target_addr Target actor address for the result.
    /// \param[in] msg_id Optional message id for correlation.
    /// \return DeliveryResult with status mapped from this code.
    /// \note The caller must include \c delivery_result.hpp for
    ///       the return type to be complete.
    DeliveryResult to_delivery_result(const ActorAddress& target_addr,
                                      MessageId msg_id = {}) const;
};

/// \brief Backpressure signal emitted when a mailbox is under pressure.
///
/// Serialized to remote producers via \c serialize_backpressure_signal() so
/// they can throttle or redirect sends. Carries depth, capacity, pressure
/// ratio, and a suggested retry interval.
struct BackpressureSignal {
    ActorAddress target; ///< Address of the actor whose mailbox is under
                         ///< pressure.
    ActorAddress sender; ///< Address of the actor that triggered the signal.
    BackpressureReason reason = BackpressureReason::HighWatermark; ///< Why the
                                                                   ///< signal
                                                                   ///< was
                                                                   ///< emitted.
    uint32_t depth = 0;          ///< Mailbox depth at signal time.
    uint32_t capacity = 0;       ///< Mailbox capacity at signal time.
    uint64_t bytes = 0;          ///< Queued byte count at signal time.
    uint64_t byte_capacity = 0;  ///< Byte capacity at signal time.
    double pressure_ratio = 0.0; ///< Pressure ratio at signal time.
    std::chrono::milliseconds retry_after{0}; ///< Suggested retry interval for
                                              ///< producers.
    uint64_t sequence = 0; ///< Monotonically increasing signal sequence number.
};

[[nodiscard]] constexpr bool is_system_message(TypeTag tag) noexcept {
    return static_cast<uint32_t>(tag) > 0 &&
           static_cast<uint32_t>(tag) < static_cast<uint32_t>(TypeTag::User);
}

[[nodiscard]] inline uint64_t
estimate_message_bytes(const TypedMessage& msg) noexcept {
    return sizeof(TypedMessage) + msg.payload().size();
}

/// \brief Human-readable snake_case string for an \c OverflowPolicy.
///
/// \param[in] policy The overflow policy value.
/// \return A null-terminated string literal. Never returns \c nullptr.
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

/// \brief Check whether a message deadline has expired.
///
/// \param[in] deadline_ns Absolute deadline in nanoseconds (monotonic clock).
///                        Use INT64_MAX for no deadline.
/// \param[in] now_ns Current monotonic timestamp in nanoseconds.
/// \return true if the deadline has passed and the message should be dropped.
/// \note Thread safety: constexpr and lock-free — safe to call from any
///       thread without synchronization.
[[nodiscard]] constexpr bool
is_expired(int64_t deadline_ns, uint64_t now_ns) noexcept {
    return deadline_ns >= 0 && deadline_ns != INT64_MAX &&
           static_cast<uint64_t>(deadline_ns) < now_ns;
}

/// \brief Human-readable snake_case string for a \c MailboxPressureState.
///
/// \param[in] state The pressure state value.
/// \return A null-terminated string literal. Never returns \c nullptr.
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
