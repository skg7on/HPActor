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

#include <hpactor/runtime/observability_runtime.hpp>

#include <hpactor/log/log_event.hpp>         // for LogEvent
#include <hpactor/log/log_manager.hpp>       // for LogManager
#include <hpactor/tracing/span.hpp>          // for SpanStart, SpanHandle
#include <hpactor/tracing/trace_manager.hpp> // for TraceManager
#include <hpactor/types/types.hpp>           // for result<T>, error, errors

namespace hpactor {

// ── Factory ────────────────────────────────────────────────────────────────

std::unique_ptr<ObservabilityRuntime>
ObservabilityRuntime::create(const ObservabilityRuntimeConfig& config) noexcept {
    return std::unique_ptr<ObservabilityRuntime>(new ObservabilityRuntime(config));
}

// ── Construction / destruction ─────────────────────────────────────────────

ObservabilityRuntime::ObservabilityRuntime(const ObservabilityRuntimeConfig& config)
    : config_(config) {
    // Derive per-subsystem configs from the blueprint config.
    metrics_config_.enabled = config.metrics_enabled;
    metrics_config_.ring_buffer_capacity = config.metrics_ring_buffer_capacity;
    logging_config_.enabled = config.logging_enabled;
    logging_config_.ring_buffer_capacity = config.logging_ring_buffer_capacity;
    tracing_config_.enabled = config.tracing_enabled;
    tracing_config_.ring_buffer_capacity = config.tracing_ring_buffer_capacity;

    // Ports are self-wired. Their identity never changes.
    metrics_port_.target = this;
    log_port_.target = this;
    trace_port_.target = this;
}

ObservabilityRuntime::~ObservabilityRuntime() {
    if (started_.load(std::memory_order_acquire)) {
        stop();
    }
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

result<void> ObservabilityRuntime::start() noexcept {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return result<void>::make();
    }

    epoch_.fetch_add(1, std::memory_order_release);

    // ── Metrics ──────────────────────────────────────────────────────────
    if (metrics_config_.enabled) {
        metrics_ring_buffer_ =
            std::make_shared<metrics::MpscRingBuffer<metrics::MetricEvent>>();
    }

    // ── Logging ──────────────────────────────────────────────────────────
    if (logging_config_.enabled) {
        log_manager_ = std::make_unique<log::LogManager>(logging_config_);
        // Defer background drain thread start: the logger is ready
        // immediately (ring buffer + formatter are created), but the
        // drain thread competes with the scheduler's timer thread for
        // CPU under coverage builds.  Start it later via
        // start_background_threads().
        logger_ = &log_manager_->logger();
    }

    // ── Tracing ──────────────────────────────────────────────────────────
    if (tracing_config_.enabled) {
        trace_manager_ =
            std::make_unique<tracing::TraceManager>(tracing_config_, nullptr);
        // Background exporter thread start deferred — see logging comment.
    }

    return result<void>::make();
}

result<void> ObservabilityRuntime::stop() noexcept {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false,
                                          std::memory_order_acq_rel)) {
        return result<void>::make();
    }

    epoch_.fetch_add(1, std::memory_order_release);

    // Reset ports to no-op state FIRST so late producers don't access
    // destroyed managers.
    metrics_port_.target = nullptr;
    log_port_.target = nullptr;
    trace_port_.target = nullptr;

    // Stop managers in reverse dependency order.
    if (trace_manager_) {
        trace_manager_->stop();
        trace_manager_.reset();
    }
    if (log_manager_) {
        log_manager_->stop();
        log_manager_.reset();
    }
    logger_ = nullptr;
    metrics_ring_buffer_.reset();

    fault_controller_.remove();

    return result<void>::make();
}

// ── Background threads ──────────────────────────────────────────────────────

void ObservabilityRuntime::start_background_threads() noexcept {
    if (log_manager_) {
        log_manager_->start();
    }
    if (trace_manager_) {
        trace_manager_->start();
    }
}

// ── Tracing reload ─────────────────────────────────────────────────────────

void ObservabilityRuntime::apply_tracing_config(const tracing::TraceConfig& config) noexcept {
    tracing_config_ = config;

    if (!tracing_config_.enabled) {
        if (trace_manager_) {
            trace_manager_->stop();
            trace_manager_.reset();
        }
        trace_port_.target = nullptr;
        return;
    }

    // For now, stop-and-recreate. Phase 6 reload transactions will make
    // this atomic with exporter drain.
    if (trace_manager_) {
        trace_manager_->stop();
        trace_manager_.reset();
    }
    trace_manager_ =
        std::make_unique<tracing::TraceManager>(tracing_config_, nullptr);
    trace_manager_->start();
}

// ── Snapshot ───────────────────────────────────────────────────────────────

ObservabilitySnapshot ObservabilityRuntime::snapshot() const noexcept {
    ObservabilitySnapshot snap;
    snap.metrics_enabled = config_.metrics_enabled;
    snap.logging_enabled = config_.logging_enabled;
    snap.tracing_enabled = config_.tracing_enabled;
    snap.fault_injection_enabled = config_.fault_injection_enabled;
    snap.metrics_drops = metrics_drops_.load(std::memory_order_acquire);
    snap.log_drops = log_drops_.load(std::memory_order_acquire);
    snap.trace_drops = trace_drops_.load(std::memory_order_acquire);
    snap.epoch = epoch_.load(std::memory_order_acquire);
    return snap;
}

// ── MetricsTarget
// ─────────────────────────────────────────────────────────────

void ObservabilityRuntime::on_metric(uint32_t event_type, uint64_t val) noexcept {
    if (metrics_ring_buffer_) {
        metrics::MetricEvent ev{};
        ev.event_type = static_cast<metrics::MetricEventType>(event_type);
        ev.value_hi = static_cast<uint32_t>(val);
        if (!metrics_ring_buffer_->try_push(ev)) {
            metrics_drops_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void ObservabilityRuntime::on_metric_event(uint32_t event_type, uint64_t val) noexcept {
    on_metric(event_type, val);
}

// ── LogTarget
// ─────────────────────────────────────────────────────────────────

void ObservabilityRuntime::on_log(const log::LogEvent& event,
                                  bool /*high_priority*/) noexcept {
    if (logger_) {
        logger_->emit(event);
    } else {
        log_drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

// ── TraceTarget
// ────────────────────────────────────────────────────────────────

tracing::SpanHandle
ObservabilityRuntime::on_span_start(const tracing::SpanStart& span) noexcept {
    if (trace_manager_) {
        return trace_manager_->start_span(span);
    }
    trace_drops_.fetch_add(1, std::memory_order_relaxed);
    return tracing::SpanHandle{};
}

void ObservabilityRuntime::on_span_finish(tracing::SpanHandle& handle,
                                          tracing::SpanStatus status) noexcept {
    if (trace_manager_) {
        trace_manager_->finish_span(handle, status);
    }
}

} // namespace hpactor
