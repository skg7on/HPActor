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

#include <hpactor/runtime/observability_dispatch_targets.hpp>

#include <cstdint>

namespace hpactor {

/// \brief Fixed-size non-owning sink for metrics events.
///
/// Stable for the lifetime of ObservabilityRuntime. When metrics are
/// disabled, the target pointer is null — callers must check before calling.
/// Sink address never changes across reload or enable/disable transitions.
struct MetricsSink {
    MetricsTarget* target{nullptr};

    /// \brief Emit a metric event. Called from any thread.
    void emit(uint32_t event_type, uint64_t val) const noexcept {
        if (target) {
            target->on_metric(event_type, val);
        }
    }

    /// \brief Emit a richer metric event with separate actor_id and value.
    void emit_event(uint32_t event_type, uint64_t val) const noexcept {
        if (target) {
            target->on_metric_event(event_type, val);
        }
    }
};

/// \brief Fixed-size non-owning sink for log events.
///
/// Stable for the lifetime of ObservabilityRuntime. When logging is
/// disabled, the target pointer is null. Sink address never changes.
struct LogSink {
    LogTarget* target{nullptr};

    /// \brief Emit a log event. Called from any thread.
    void emit(const log::LogEvent& event, bool high_priority) const noexcept {
        if (target) {
            target->on_log(event, high_priority);
        }
    }
};

/// \brief Fixed-size non-owning sink for trace span recording.
///
/// Stable for the lifetime of ObservabilityRuntime. When tracing is
/// disabled, the target pointer is null. Sink address never changes.
struct TraceSink {
    TraceTarget* target{nullptr};

    /// \brief Record a new span. Called from any thread.
    tracing::SpanHandle record_span(const tracing::SpanStart& span) const noexcept {
        if (target) {
            return target->on_span_start(span);
        }
        return tracing::SpanHandle{};
    }

    /// \brief Finish a span. Called from any thread.
    void finish_span(tracing::SpanHandle& handle,
                     tracing::SpanStatus status) const noexcept {
        if (target) {
            target->on_span_finish(handle, status);
        }
    }
};

} // namespace hpactor
