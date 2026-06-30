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

#include "runtime_blueprint.hpp"

#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/log/log_config.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/metrics/metrics_config.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/runtime/observability_ports.hpp>
#include <hpactor/tracing/trace_config.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <memory>

namespace hpactor {

namespace log {
class LogManager;
} // namespace log

namespace tracing {
class TraceManager;
} // namespace tracing

/// \brief Immutable snapshot of observability runtime state.
struct ObservabilitySnapshot final {
    bool metrics_enabled{false};
    bool logging_enabled{false};
    bool tracing_enabled{false};
    bool fault_injection_enabled{false};
    uint64_t metrics_drops{0};
    uint64_t log_drops{0};
    uint64_t trace_drops{0};
    uint64_t epoch{0};
};

/// \brief Cohesive owner of metrics, logging, tracing, and fault injection
/// infrastructure.
class ObservabilityRuntime final {
  public:
    static std::unique_ptr<ObservabilityRuntime>
    create(const ObservabilityRuntimeConfig& config) noexcept;

    ~ObservabilityRuntime();

    // ── Lifecycle ────────────────────────────────────────────────────────
    result<void> start() noexcept;
    result<void> stop() noexcept;

    // ── Stable ports ─────────────────────────────────────────────────────
    const MetricsSinkPort& metrics_sink() const noexcept {
        return metrics_port_;
    }
    const LogSinkPort& log_sink() const noexcept {
        return log_port_;
    }
    const TraceSinkPort& trace_sink() const noexcept {
        return trace_port_;
    }

    // ── Resource accessors (for wiring producers) ─────────────────────────
    metrics::MpscRingBuffer<metrics::MetricEvent>*
    metrics_ring_buffer() const noexcept {
        return metrics_ring_buffer_.get();
    }
    log::Logger* logger() const noexcept {
        return logger_;
    }
    log::LogManager* log_manager() const noexcept {
        return log_manager_.get();
    }
    tracing::TraceManager* trace_manager() const noexcept {
        return trace_manager_.get();
    }
    fault::FaultController& fault_controller() noexcept {
        return fault_controller_;
    }

    // ── Configuration ────────────────────────────────────────────────────
    const metrics::MetricsConfig& metrics_config() const noexcept {
        return metrics_config_;
    }
    const log::LogConfig& logging_config() const noexcept {
        return logging_config_;
    }
    const tracing::TraceConfig& tracing_config() const noexcept {
        return tracing_config_;
    }

    /// \brief Apply a new tracing configuration (reload).
    void apply_tracing_config(const tracing::TraceConfig& config) noexcept;

    // ── Observability ────────────────────────────────────────────────────
    ObservabilitySnapshot snapshot() const noexcept;

  private:
    explicit ObservabilityRuntime(const ObservabilityRuntimeConfig& config);

    ObservabilityRuntimeConfig config_;

    // Per-subsystem config values (populated from config_ at construction,
    // updated by apply_tracing_config / reconfigure).
    metrics::MetricsConfig metrics_config_;
    log::LogConfig logging_config_;
    tracing::TraceConfig tracing_config_;

    // Owned infrastructure — created in start(), destroyed in stop().
    std::shared_ptr<metrics::MpscRingBuffer<metrics::MetricEvent>> metrics_ring_buffer_;
    std::unique_ptr<log::LogManager> log_manager_;
    log::Logger* logger_{nullptr}; // points into log_manager_
    std::unique_ptr<tracing::TraceManager> trace_manager_;
    fault::FaultController fault_controller_;

    // Stable ports — identity never changes.
    MetricsSinkPort metrics_port_;
    LogSinkPort log_port_;
    TraceSinkPort trace_port_;

    std::atomic<bool> started_{false};
    std::atomic<uint64_t> metrics_drops_{0};
    std::atomic<uint64_t> log_drops_{0};
    std::atomic<uint64_t> trace_drops_{0};
    std::atomic<uint64_t> epoch_{0};
};

} // namespace hpactor
