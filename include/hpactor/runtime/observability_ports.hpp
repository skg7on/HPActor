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

#include <hpactor/tracing/span.hpp> // for SpanStart, SpanHandle, SpanStatus

#include <cstdint>

namespace hpactor {

namespace log {
struct LogEvent;
} // namespace log

/// \brief Fixed-size non-owning sink for metrics events.
///
/// Stable for the lifetime of ObservabilityRuntime. When metrics are
/// disabled, `emit` is null — callers must check before calling.
/// Port address never changes across reload or enable/disable transitions.
struct MetricsSinkPort {
    void* context{nullptr};

    /// \brief Emit a metric event. Called from any thread.
    /// \param ctx The context pointer (ObservabilityRuntime*).
    /// \param event_type Metric event type code.
    /// \param val Auxiliary value (actor_id, size, count, etc.).
    void (*emit)(void* ctx, uint32_t event_type, uint64_t val) noexcept {nullptr};

    /// \brief Emit a richer metric event with separate actor_id and value.
    /// Used by producers that already have the actor_id available.
    void (*emit_event)(void* ctx, uint32_t event_type,
                       uint64_t val) noexcept {nullptr};
};

/// \brief Fixed-size non-owning sink for log events.
///
/// Stable for the lifetime of ObservabilityRuntime. When logging is
/// disabled, `emit` is null. Port address never changes.
struct LogSinkPort {
    void* context{nullptr};

    /// \brief Emit a log event. Called from any thread.
    /// \param ctx The context pointer.
    /// \param event The log event to record.
    /// \param high_priority If true, the event bypasses the ring buffer if full
    ///        (used for Critical/Fatal levels).
    void (*emit)(void* ctx, const log::LogEvent& event,
                 bool high_priority) noexcept {nullptr};
};

/// \brief Fixed-size non-owning sink for trace span recording.
///
/// Stable for the lifetime of ObservabilityRuntime. When tracing is
/// disabled, `record_span` is null. Port address never changes.
struct TraceSinkPort {
    void* context{nullptr};

    /// \brief Record a new span. Called from any thread.
    /// \param ctx The context pointer.
    /// \param span The span start record (includes parent trace context
    ///        in \c span.parent).
    /// \return A span handle for the newly created span, or a default/null
    ///         handle if tracing is disabled or the ring buffer is full.
    tracing::SpanHandle (*record_span)(void* ctx,
                                       const tracing::SpanStart& span) noexcept {
        nullptr};

    /// \brief Finish a span. Called from any thread.
    /// \param ctx The context pointer.
    /// \param handle The span handle to finish (in/out — cleared on
    ///        completion).
    /// \param status The span status code.
    void (*finish_span)(void* ctx, tracing::SpanHandle& handle,
                        tracing::SpanStatus status) noexcept {nullptr};
};

} // namespace hpactor
