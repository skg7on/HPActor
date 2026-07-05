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

#include <hpactor/tracing/span.hpp>

#include <cstdint>

namespace hpactor {

namespace log {
struct LogEvent; ///< Full definition in <hpactor/log/log_event.hpp>
} // namespace log

// ── Metrics dispatch ──────────────────────────────────────────────────────

/// \brief Typed target for metrics events.
///
/// Implemented by the metrics aggregation subsystem
/// (ObservabilityRuntime).  Stable for the lifetime of the runtime;
/// sink address never changes across reload or enable/disable transitions.
class MetricsTarget {
  public:
    virtual ~MetricsTarget() = default;

    /// \brief Emit a metric event.  Called from any thread.
    ///
    /// \param event_type Metric event type code.
    /// \param val        Auxiliary value (actor_id, size, count, etc.).
    virtual void on_metric(uint32_t event_type, uint64_t val) noexcept = 0;

    /// \brief Emit a richer metric event with separate actor_id and value.
    ///
    /// Used by producers that already have the actor_id available.
    virtual void on_metric_event(uint32_t event_type, uint64_t val) noexcept = 0;
};

// ── Log dispatch ──────────────────────────────────────────────────────────

/// \brief Typed target for structured log events.
///
/// Implemented by the logging subsystem (LogManager).  Stable for the
/// lifetime of ObservabilityRuntime.  When logging is disabled, the
/// target pointer is null.
class LogTarget {
  public:
    virtual ~LogTarget() = default;

    /// \brief Emit a log event.  Called from any thread.
    ///
    /// \param event         The log event to record.
    /// \param high_priority If true, the event bypasses the ring buffer if
    ///                      full (used for Critical/Fatal levels).
    virtual void
    on_log(const log::LogEvent& event, bool high_priority) noexcept = 0;
};

// ── Trace dispatch ────────────────────────────────────────────────────────

/// \brief Typed target for distributed trace span recording.
///
/// Implemented by the tracing subsystem (TraceManager).  Stable for the
/// lifetime of ObservabilityRuntime.  When tracing is disabled, the
/// target pointer is null.
class TraceTarget {
  public:
    virtual ~TraceTarget() = default;

    /// \brief Record a new span.  Called from any thread.
    ///
    /// \param span The span start record (includes parent trace context
    ///             in \c span.parent).
    /// \return A span handle for the newly created span, or a default/null
    ///         handle if tracing is disabled or the ring buffer is full.
    virtual tracing::SpanHandle
    on_span_start(const tracing::SpanStart& span) noexcept = 0;

    /// \brief Finish a span.  Called from any thread.
    ///
    /// \param handle The span handle to finish (in/out — cleared on
    ///               completion).
    /// \param status The span status code.
    virtual void on_span_finish(tracing::SpanHandle& handle,
                                tracing::SpanStatus status) noexcept = 0;
};

} // namespace hpactor
