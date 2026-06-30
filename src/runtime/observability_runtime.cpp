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

#include "observability_runtime.hpp"

#include <hpactor/log/log_event.hpp> // for LogEvent
#include <hpactor/tracing/span.hpp>  // for SpanStart, SpanHandle
#include <hpactor/types/types.hpp>   // for result<T>, error, errors

namespace hpactor {

// ── Factory ────────────────────────────────────────────────────────────────

std::unique_ptr<ObservabilityRuntime>
ObservabilityRuntime::create(const ObservabilityRuntimeConfig& config) noexcept {
    return std::unique_ptr<ObservabilityRuntime>(new ObservabilityRuntime(config));
}

// ── Construction / destruction ─────────────────────────────────────────────

ObservabilityRuntime::ObservabilityRuntime(const ObservabilityRuntimeConfig& config)
    : config_(config) {
    // Ports are wired to internal handlers. When disabled, the ports
    // increment drop counters. When enabled, they deliver to ring buffers.
    //
    // The port objects themselves are stable — their addresses never change.

    // Metrics port — self-wired to internal state.
    metrics_port_.context = this;

    // Log port — self-wired.
    log_port_.context = this;

    // Trace port — self-wired.
    trace_port_.context = this;
}

ObservabilityRuntime::~ObservabilityRuntime() {
    // Ensure stop is called if the runtime was started.
    // Ignore the result — we can't throw from a destructor.
    if (started_.load(std::memory_order_acquire)) {
        stop();
    }
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

result<void> ObservabilityRuntime::start() noexcept {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        // Already started — idempotent, return success.
        return result<void>::make();
    }

    epoch_.fetch_add(1, std::memory_order_release);

    // Wire ports based on config. When disabled, ports remain no-ops.
    // When enabled, they deliver to the ring buffers created below.
    //
    // For now, we only wire the ports — actual ring buffer creation and
    // manager startup is done in Tasks 3-4 when metrics/log/tracing
    // infrastructure is moved in.
    if (config_.metrics_enabled) {
        metrics_port_.emit = [](void* ctx, uint32_t /*event_type*/,
                                uint64_t /*val*/) noexcept {
            // Placeholder — will deliver to shared ring buffer in Task 3.
            (void)ctx;
        };
    }

    if (config_.logging_enabled) {
        log_port_.emit = [](void* ctx, const log::LogEvent& /*event*/,
                            bool /*high_priority*/) noexcept {
            // Placeholder — will deliver to log ring buffer in Task 4.
            (void)ctx;
        };
    }

    if (config_.tracing_enabled) {
        trace_port_.record_span =
            [](void* ctx, const tracing::SpanStart& /*span*/,
               tracing::SpanHandle /*parent*/) noexcept -> tracing::SpanHandle {
            // Placeholder — will deliver to trace manager in Task 4.
            (void)ctx;
            return tracing::SpanHandle{};
        };
        trace_port_.finish_span = [](void* ctx, tracing::SpanHandle /*handle*/,
                                     uint32_t /*status*/) noexcept {
            // Placeholder.
            (void)ctx;
        };
    }

    return result<void>::make();
}

result<void> ObservabilityRuntime::stop() noexcept {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false,
                                          std::memory_order_acq_rel)) {
        // Already stopped or never started — idempotent.
        return result<void>::make();
    }

    epoch_.fetch_add(1, std::memory_order_release);

    // Reset ports to no-op state.
    metrics_port_.emit = nullptr;
    metrics_port_.emit_event = nullptr;
    log_port_.emit = nullptr;
    trace_port_.record_span = nullptr;
    trace_port_.finish_span = nullptr;

    return result<void>::make();
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

} // namespace hpactor
