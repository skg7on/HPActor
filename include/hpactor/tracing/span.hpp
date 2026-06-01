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

#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string_view>

namespace hpactor::tracing {

/// \brief Span kind classification (W3C trace context semantics).
enum class SpanKind : uint8_t {
    kInternal, ///< Internal operation within a service.
    kServer,   ///< Server-side handling of an incoming request.
    kClient,   ///< Client-side outgoing request.
    kProducer, ///< Message producer (sender).
    kConsumer, ///< Message consumer (receiver).
};

/// \brief Span completion status.
enum class SpanStatus : uint8_t {
    kUnset, ///< Status not explicitly set.
    kOk,    ///< Operation completed successfully.
    kError, ///< Operation completed with an error.
};

/// \brief Parameters for starting a new span.
///
/// Passed to TraceManager::start_span().
struct SpanStart {
    /// \brief Span name (typically the message handler or operation name).
    std::string_view name;
    /// \brief Span kind.
    SpanKind kind{SpanKind::kInternal};
    /// \brief Parent trace context. Zeroed if this is a root span.
    TraceContext parent{};
    /// \brief Whether this span has a parent context.
    bool has_parent{false};
    /// \brief Actor associated with this span.
    ActorId actor_id{};
    /// \brief Sender actor (for consumer spans tracking message provenance).
    ActorId sender_actor_id{};
    /// \brief Message type tag (for consumer spans).
    TypeTag type_tag{TypeTag::Invalid};
    /// \brief Message identifier (for consumer spans).
    MessageId message_id{};
    /// \brief Payload size in bytes (for consumer spans).
    uint32_t payload_size{0};
};

/// \brief Handle to an in-flight span, returned by start_span().
///
/// Must be passed to finish_span() to record completion. Not movable
/// or copyable between threads.
struct SpanHandle {
    /// \brief Trace context (trace id + span id + flags).
    TraceContext context{};
    /// \brief Parent span id. Zero for root spans.
    SpanId parent_span_id{};
    /// \brief Monotonic start timestamp in nanoseconds.
    uint64_t start_ns{0};
    /// \brief Span kind.
    SpanKind kind{SpanKind::kInternal};
    /// \brief Actor that owns this span.
    ActorId actor_id{};
    /// \brief Sender actor id.
    ActorId sender_actor_id{};
    /// \brief Message type tag.
    TypeTag type_tag{TypeTag::Invalid};
    /// \brief Message id.
    MessageId message_id{};
    /// \brief Payload size.
    uint32_t payload_size{0};
    /// \brief Whether the sampling decision was to record this span.
    bool recording{false};
};

/// \brief Completed span ready for export.
///
/// Enqueued into the trace manager's ring buffer by finish_span()
/// when recording is enabled.
struct SpanRecord {
    /// \brief W3C trace id (16 bytes).
    TraceId trace_id;
    /// \brief W3C span id (8 bytes).
    SpanId span_id;
    /// \brief Parent span id. Zero for root spans.
    SpanId parent_span_id;
    /// \brief Actor that owned the span.
    ActorId actor_id;
    /// \brief Sender actor id.
    ActorId sender_actor_id;
    /// \brief Message type tag (encoded as uint32_t for wire format).
    uint32_t type_tag{0};
    /// \brief Message id (encoded as uint64_t).
    uint64_t message_id{0};
    /// \brief Span start time in nanoseconds.
    uint64_t start_ns{0};
    /// \brief Span end time in nanoseconds.
    uint64_t end_ns{0};
    /// \brief Payload size in bytes.
    uint32_t payload_size{0};
    /// \brief Span kind.
    SpanKind kind{SpanKind::kInternal};
    /// \brief Span completion status.
    SpanStatus status{SpanStatus::kUnset};
    /// \brief Bitmask of populated optional attributes.
    uint16_t attribute_mask{0};
};

} // namespace hpactor::tracing
