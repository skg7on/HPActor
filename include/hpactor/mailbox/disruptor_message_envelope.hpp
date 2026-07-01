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

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <type_traits>
#include <variant>

namespace hpactor::mailbox {

// ── Disruptor-message concept
// ─────────────────────────────────────────────────

/// \brief Concept for messages suitable for fixed-mailbox ring storage.
///
/// Messages must have a fixed object representation with no owning
/// pointers, dynamic allocations, or nontrivial lifetime management.
/// This makes copying into a claimed slot non-throwing and prevents
/// a \c std::string, \c std::vector, protobuf object, or other
/// variable-length RAII value from masquerading as a fixed message.
///
/// \note Raw pointers satisfy the concept but are subject to the
///       actor rule that mutable shared state must not cross actor
///       boundaries. Prefer IDs, offsets, handles, or immutable views
///       with externally guaranteed lifetime.
template <typename T>
concept DisruptorMessage =
    std::is_standard_layout_v<T> && std::is_trivially_default_constructible_v<T> &&
    std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

// ── Unique-type helper ────────────────────────────────────────────────────

namespace detail {

template <typename...> struct are_unique : std::true_type {};

template <typename T, typename... Rest>
struct are_unique<T, Rest...>
    : std::conjunction<std::negation<std::disjunction<std::is_same<T, Rest>...>>,
                       are_unique<Rest...>> {};

template <typename... Types>
inline constexpr bool are_unique_v = are_unique<Types...>::value;

template <typename T, typename... Rest> struct one_of : std::false_type {};

template <typename T, typename First, typename... Rest>
struct one_of<T, First, Rest...>
    : std::disjunction<std::is_same<std::remove_cvref_t<T>, First>, one_of<T, Rest...>> {
};

template <typename T, typename First>
struct one_of<T, First> : std::is_same<std::remove_cvref_t<T>, First> {};

template <typename T, typename... Options>
inline constexpr bool one_of_v = one_of<T, Options...>::value;

} // namespace detail

// ── Send options ──────────────────────────────────────────────────────────

/// \brief Delivery options for a fixed-message send.
///
/// Callers supply these when calling \c try_send().  \c ActorContext
/// populates the sender address, trace, and message-id fields
/// automatically; direct-reference callers provide only the
/// application-visible options here.
struct DisruptorSendOptions {
    /// Deadline as a monotonic nanosecond timestamp.
    /// \c INT64_MAX means no deadline.
    int64_t deadline_ns{INT64_MAX};

    /// Caller-assigned message identifier (0 = auto-assign by context).
    uint64_t message_id{0};

    /// Caller-defined flags (reserved, must be 0 in version 1).
    uint32_t flags{0};
};

// ── Envelope metadata ─────────────────────────────────────────────────────

/// \brief Delivery metadata populated by \c ActorContext.
///
/// Every fixed-message envelope carries this metadata alongside the
/// variant message payload.  The metadata is populated before the
/// message is published to the ring and is stable through handler
/// dispatch.
struct DisruptorEnvelopeMeta {
    /// Sender's actor address (empty for non-actor callers).
    ActorAddress sender{};

    /// W3C trace context for distributed tracing.
    TraceContext trace{};

    /// Monotonic nanosecond deadline (INT64_MAX = none).
    int64_t deadline_ns{INT64_MAX};

    /// Unique message identifier for deduplication and tracking.
    uint64_t message_id{0};

    /// Assigned logical producer sequence in the ring.
    /// Set by the mailbox core after successful publication.
    uint64_t enqueue_sequence{0};

    /// Reserved flags (must be 0 in version 1).
    uint32_t flags{0};

    /// True when \c trace contains a valid W3C trace context.
    bool has_trace{false};
};

// ── Disruptor message envelope
// ────────────────────────────────────────────────

/// \brief Compile-time fixed envelope for ring storage.
///
/// Contains a closed \c std::variant of the allowed message types and
/// the delivery metadata populated by \c ActorContext.  The envelope
/// size is determined by the largest alternative plus the variant
/// discriminator and metadata — there is no dynamic payload.
///
/// \tparam Messages The closed set of fixed-message types.  Each must
///         satisfy \c DisruptorMessage and must be unique.
template <DisruptorMessage... Messages> struct DisruptorMessageEnvelope {
    static_assert(sizeof...(Messages) > 0,
                  "DisruptorMessageEnvelope requires at least one message type");
    static_assert(detail::are_unique_v<Messages...>,
                  "DisruptorMessageEnvelope message types must be unique");

    /// The closed variant of allowed message types.
    using message_type = std::variant<Messages...>;

    /// The user-message payload.
    message_type message{};

    /// Delivery metadata.
    DisruptorEnvelopeMeta meta{};
};

} // namespace hpactor::mailbox
