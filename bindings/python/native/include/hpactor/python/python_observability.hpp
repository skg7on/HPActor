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

#include <hpactor/log/logger.hpp>
#include <hpactor/metrics/metrics_registry.hpp>
#include <hpactor/tracing/trace_manager.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor::python {

/// \brief Start value for a Python handler child span.
struct PythonHandlerSpanStart final {
    TraceContext parent;
    std::string actor_type;
    uint64_t generation{0};
    uint64_t message_id{0};
    uint32_t type_tag{0};
    uint64_t dispatch_enqueue_ns{0};
    uint64_t handler_start_ns{0};
};

/// \brief Status applied at span finish.
enum class PythonHandlerSpanStatus : uint8_t {
    Ok = 0,
    Error = 1,
    Cancelled = 2,
    Expired = 3,
    Stale = 4,
};

/// \brief Owned observability surface for the Python binding subsystem.
///
/// Registers the approved metric families, emits bounded structured logs,
/// and manages handler child spans. All methods are noexcept and never
/// change actor behavior when the underlying metric/log/trace subsystem
/// is unavailable.
class PythonObservability final {
  public:
    /// \brief Dependencies for this observability instance.
    struct Dependencies final {
        metrics::MetricRegistry* metrics{nullptr};
        log::Logger* logger{nullptr};
        tracing::TraceManager* tracer{nullptr};
    };

    explicit PythonObservability(Dependencies deps) noexcept;

    /// \brief Whether metrics registration succeeded.
    [[nodiscard]] bool has_metrics() const noexcept {
        return metrics_ != nullptr;
    }

    /// \brief Whether logging is available.
    [[nodiscard]] bool has_logger() const noexcept {
        return logger_ != nullptr;
    }

    /// \brief Whether tracing is available.
    [[nodiscard]] bool has_tracer() const noexcept {
        return tracer_ != nullptr;
    }

    // ── Metric recording ─────────────────────────────────────────────────

    void record_dispatch(const std::string& actor_type, bool accepted) noexcept;
    void record_dispatch_rejected(const std::string& actor_type) noexcept;
    void record_command(bool accepted) noexcept;
    void record_command_rejected() noexcept;
    void record_handler(const std::string& actor_type,
                        std::chrono::microseconds duration, bool exception) noexcept;
    void record_handler_exception(const std::string& actor_type) noexcept;
    void record_handler_cancelled(const std::string& actor_type) noexcept;
    void record_loop_lag(std::chrono::microseconds lag) noexcept;
    void record_stale_completion() noexcept;

    // ── Structured logging ───────────────────────────────────────────────

    void log_handler_failure(const std::string& actor_type,
                             const std::string& exception_type,
                             const std::string& detail,
                             const std::string& traceback) noexcept;

    // ── Span management ──────────────────────────────────────────────────

    [[nodiscard]] uint64_t
    begin_handler_span(const PythonHandlerSpanStart& start) noexcept;
    void
    finish_handler_span(uint64_t token, PythonHandlerSpanStatus status) noexcept;

  private:
    metrics::MetricRegistry* metrics_{nullptr};
    log::Logger* logger_{nullptr};
    tracing::TraceManager* tracer_{nullptr};
};

} // namespace hpactor::python
