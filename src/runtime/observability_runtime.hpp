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

#include <hpactor/runtime/observability_ports.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <memory>

namespace hpactor {

/// \brief Immutable snapshot of observability runtime state.
///
/// Copied values only — no locks held after construction. Carries
/// per-component enabled flags and bounded counter values.
struct ObservabilitySnapshot final {
    bool metrics_enabled{false};
    bool logging_enabled{false};
    bool tracing_enabled{false};
    bool fault_injection_enabled{false};

    /// \brief Metrics drop counter (events discarded because ring buffer
    /// was full or metrics were disabled).
    uint64_t metrics_drops{0};

    /// \brief Log drop counter.
    uint64_t log_drops{0};

    /// \brief Trace drop counter.
    uint64_t trace_drops{0};

    /// \brief Lifecycle epoch of the observability runtime when snapshot was
    /// taken.
    uint64_t epoch{0};
};

/// \brief Cohesive owner of metrics, logging, tracing, and fault injection
/// infrastructure.
///
/// All telemetry producers receive stable ports that never change identity
/// across enable/disable/reload transitions. Ports close only after all
/// producers have quiesced.
///
/// \note Actor objects used for metrics aggregation (MetricsActor) remain
/// owned by ActorRuntime. ObservabilityRuntime owns only the storage,
/// exporters, and port infrastructure.
class ObservabilityRuntime final {
  public:
    /// \brief Create an ObservabilityRuntime from validated config.
    ///
    /// Construction is side-effect-free: no threads, ring buffers, actors,
    /// or exporters are created until \c start() is called.
    static std::unique_ptr<ObservabilityRuntime>
    create(const ObservabilityRuntimeConfig& config) noexcept;

    /// \brief Destroy the runtime.
    ///
    /// If the runtime has been started but not stopped, stop() is called
    /// before destruction.
    ~ObservabilityRuntime();

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /// \brief Start all enabled telemetry subsystems.
    ///
    /// Creates ring buffers, exporters, and manager objects. Idempotent
    /// when already started.
    result<void> start() noexcept;

    /// \brief Stop all telemetry subsystems.
    ///
    /// Closes ports, drains exporters, and destroys manager objects.
    /// Idempotent when already stopped.
    result<void> stop() noexcept;

    // ── Stable ports ───────────────────────────────────────────────────────

    /// \brief Metrics sink port (stable identity, never null).
    const MetricsSinkPort& metrics_sink() const noexcept {
        return metrics_port_;
    }

    /// \brief Log sink port (stable identity, never null).
    const LogSinkPort& log_sink() const noexcept {
        return log_port_;
    }

    /// \brief Trace sink port (stable identity, never null).
    const TraceSinkPort& trace_sink() const noexcept {
        return trace_port_;
    }

    // ── Observability ──────────────────────────────────────────────────────

    /// \brief Capture a bounded read-only snapshot of observability state.
    ///
    /// The snapshot carries per-component enabled flags, drop counters, and
    /// a lifecycle epoch. No locks are held after the snapshot is returned.
    ObservabilitySnapshot snapshot() const noexcept;

  private:
    explicit ObservabilityRuntime(const ObservabilityRuntimeConfig& config);

    ObservabilityRuntimeConfig config_;

    // Stable ports — identity never changes.
    MetricsSinkPort metrics_port_;
    LogSinkPort log_port_;
    TraceSinkPort trace_port_;

    // Lifecycle state.
    std::atomic<bool> started_{false};

    // Drop counters (atomic, incremented when ports are called while disabled
    // or when ring buffers are full).
    std::atomic<uint64_t> metrics_drops_{0};
    std::atomic<uint64_t> log_drops_{0};
    std::atomic<uint64_t> trace_drops_{0};
    std::atomic<uint64_t> epoch_{0};
};

} // namespace hpactor
