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
///
/// Captured when the Python runtime begins executing a handler so the
/// observability layer can create a child span from the parent trace context.
struct PythonHandlerSpanStart final {
    TraceContext parent;                   ///< Parent trace context from the incoming message.
    std::string actor_type;                ///< Actor type label for span attributes.
    uint64_t generation{0};                ///< Actor generation at dispatch time.
    uint64_t message_id{0};                ///< Incoming message identifier.
    uint32_t type_tag{0};                  ///< Message TypeTag for span attributes.
    uint64_t dispatch_enqueue_ns{0};       ///< Monotonic timestamp when the dispatch was enqueued.
    uint64_t handler_start_ns{0};          ///< Monotonic timestamp when the handler began execution.
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

    /// \brief Construct the observability surface with optional subsystems.
    ///
    /// \param[in] deps Dependencies for metrics, logging, and tracing. Any
    ///                 nullptr dep gracefully disables that subsystem.
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

    /// \brief Record a dispatch attempt with acceptance status.
    ///
    /// \param[in] actor_type The Python actor's type name for metric labels.
    /// \param[in] accepted Whether the dispatch was enqueued (true) or
    ///                     rejected due to a full queue (false).
    void record_dispatch(const std::string& actor_type, bool accepted) noexcept;

    /// \brief Record a dispatch rejected due to a full ring buffer.
    ///
    /// \param[in] actor_type The Python actor's type name for metric labels.
    void record_dispatch_rejected(const std::string& actor_type) noexcept;

    /// \brief Record a command submission with acceptance status.
    ///
    /// \param[in] accepted Whether the command was enqueued successfully.
    void record_command(bool accepted) noexcept;

    /// \brief Record a command rejected due to a full command queue.
    void record_command_rejected() noexcept;

    /// \brief Record a handler execution with duration and outcome.
    ///
    /// \param[in] actor_type The Python actor's type name for metric labels.
    /// \param[in] duration Wall-clock duration of the handler invocation.
    /// \param[in] exception Whether the handler raised an unhandled exception.
    void record_handler(const std::string& actor_type,
                        std::chrono::microseconds duration, bool exception) noexcept;

    /// \brief Record an unhandled exception in a handler.
    ///
    /// \param[in] actor_type The Python actor's type name for metric labels.
    void record_handler_exception(const std::string& actor_type) noexcept;

    /// \brief Record a handler cancelled due to message deadline expiry.
    ///
    /// \param[in] actor_type The Python actor's type name for metric labels.
    void record_handler_cancelled(const std::string& actor_type) noexcept;

    /// \brief Record the observed event-loop lag.
    ///
    /// \param[in] lag Time between dispatch enqueue and handler start.
    void record_loop_lag(std::chrono::microseconds lag) noexcept;

    /// \brief Record a stale completion that no awaited future accepted.
    void record_stale_completion() noexcept;

    // ── Structured logging ───────────────────────────────────────────────

    /// \brief Emit a structured log entry for a handler failure.
    ///
    /// \param[in] actor_type The Python actor's type name.
    /// \param[in] exception_type Python fully-qualified exception class name
    ///                          (max 255 bytes).
    /// \param[in] detail Bounded detail string (max 4096 bytes).
    /// \param[in] traceback Python traceback as a string (max 16384 bytes).
    void log_handler_failure(const std::string& actor_type,
                             const std::string& exception_type,
                             const std::string& detail,
                             const std::string& traceback) noexcept;

    // ── Span management ──────────────────────────────────────────────────

    /// \brief Begin a child span for a Python handler invocation.
    ///
    /// \param[in] start Span start metadata including parent trace context.
    /// \return An opaque span token for use with finish_handler_span(), or 0
    ///         if tracing is disabled.
    [[nodiscard]] uint64_t
    begin_handler_span(const PythonHandlerSpanStart& start) noexcept;

    /// \brief Finish a handler child span.
    ///
    /// \param[in] token The span token returned by begin_handler_span().
    /// \param[in] status The span outcome (Ok, Error, Cancelled, Expired,
    ///                   or Stale).
    void
    finish_handler_span(uint64_t token, PythonHandlerSpanStatus status) noexcept;

  private:
    metrics::MetricRegistry* metrics_{nullptr};
    log::Logger* logger_{nullptr};
    tracing::TraceManager* tracer_{nullptr};
};

} // namespace hpactor::python
