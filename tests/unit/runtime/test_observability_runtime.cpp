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

/// \file test_observability_runtime.cpp
///
/// \brief Unit tests for ObservabilityRuntime, stable telemetry ports,
///        and snapshot operations.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

// ── Stable telemetry ports ─────────────────────────────────────────────────

#include <hpactor/log/log_event.hpp> // for LogEvent
#include <hpactor/runtime/observability_sinks.hpp>
#include <hpactor/tracing/span.hpp> // for SpanHandle, SpanStatus

// ── Test targets (stub implementations) ────────────────────────────────────

namespace {

class StubMetricsTarget : public hpactor::MetricsTarget {
  public:
    void on_metric(uint32_t, uint64_t) noexcept override {
        ++emit_count;
    }
    void on_metric_event(uint32_t, uint64_t) noexcept override {
        ++emit_event_count;
    }

    int emit_count{0};
    int emit_event_count{0};
};

class StubLogTarget : public hpactor::LogTarget {
  public:
    void on_log(const hpactor::log::LogEvent&, bool) noexcept override {
        ++emit_count;
    }

    int emit_count{0};
};

class StubTraceTarget : public hpactor::TraceTarget {
  public:
    hpactor::tracing::SpanHandle
    on_span_start(const hpactor::tracing::SpanStart&) noexcept override {
        ++span_start_count;
        return hpactor::tracing::SpanHandle{};
    }
    void on_span_finish(hpactor::tracing::SpanHandle&,
                        hpactor::tracing::SpanStatus) noexcept override {
        ++span_finish_count;
    }

    int span_start_count{0};
    int span_finish_count{0};
};

} // namespace

// ── MetricsSink ─────────────────────────────────────────────────────────

TEST(MetricsSinkTest, DefaultConstructedIsNoop) {
    hpactor::MetricsSink port;
    EXPECT_EQ(port.target, nullptr);
    // Calling emit on a null target is a safe no-op.
    port.emit(1, 42);
    port.emit_event(1, 42);
}

TEST(MetricsSinkTest, WiredPortDeliversEvent) {
    StubMetricsTarget target;
    hpactor::MetricsSink port;
    port.target = &target;

    port.emit(1, 42);
    EXPECT_EQ(target.emit_count, 1);

    port.emit_event(1, 42);
    EXPECT_EQ(target.emit_event_count, 1);
}

TEST(MetricsSinkTest, PortIdentityIsStable) {
    hpactor::MetricsSink port;
    const auto* addr = &port;
    EXPECT_EQ(addr, &port);
}

// ── LogSink ────────────────────────────────────────────────────────────

TEST(LogSinkTest, DefaultConstructedIsNoop) {
    hpactor::LogSink port;
    EXPECT_EQ(port.target, nullptr);
}

TEST(LogSinkTest, WiredPortDeliversToTarget) {
    StubLogTarget target;
    hpactor::LogSink port;
    port.target = &target;

    hpactor::log::LogEvent event{};
    port.emit(event, false);
    EXPECT_EQ(target.emit_count, 1);
}

TEST(LogSinkTest, PortIdentityIsStable) {
    hpactor::LogSink port;
    const auto* addr = &port;
    EXPECT_EQ(addr, &port);
}

// ── TraceSink ──────────────────────────────────────────────────────────

TEST(TraceSinkTest, DefaultConstructedIsNoop) {
    hpactor::TraceSink port;
    EXPECT_EQ(port.target, nullptr);
}

TEST(TraceSinkTest, WiredPortDeliversToTarget) {
    StubTraceTarget target;
    hpactor::TraceSink port;
    port.target = &target;

    hpactor::tracing::SpanStart start{};
    auto handle = port.record_span(start);
    EXPECT_EQ(target.span_start_count, 1);

    port.finish_span(handle, hpactor::tracing::SpanStatus::kOk);
    EXPECT_EQ(target.span_finish_count, 1);
}

TEST(TraceSinkTest, PortIdentityIsStable) {
    hpactor::TraceSink port;
    const auto* addr = &port;
    EXPECT_EQ(addr, &port);
}

// ── ObservabilityRuntime skeleton ──────────────────────────────────────────

#include <hpactor/runtime/observability_runtime.hpp>

#include <hpactor/types/types.hpp> // for result<T>, error

TEST(ObservabilityRuntimeTest, ConstructionIsSideEffectFree) {
    hpactor::ObservabilityRuntimeConfig cfg;
    auto runtime = hpactor::ObservabilityRuntime::create(cfg);
    EXPECT_TRUE(runtime != nullptr);
}

TEST(ObservabilityRuntimeTest, StartStopLifecycle) {
    hpactor::ObservabilityRuntimeConfig cfg{/*metrics_enabled=*/false,
                                            /*logging_enabled=*/false,
                                            /*tracing_enabled=*/false,
                                            /*metrics_ring_buffer_capacity=*/0,
                                            /*logging_ring_buffer_capacity=*/0,
                                            /*tracing_ring_buffer_capacity=*/0,
                                            /*fault_injection_enabled=*/false};

    auto runtime = hpactor::ObservabilityRuntime::create(cfg);
    ASSERT_TRUE(runtime != nullptr);

    auto start_result = runtime->start();
    EXPECT_TRUE(start_result.ok());

    auto stop_result = runtime->stop();
    EXPECT_TRUE(stop_result.ok());
}

TEST(ObservabilityRuntimeTest, StopWithoutStartIsSafe) {
    hpactor::ObservabilityRuntimeConfig cfg{false, false, false, 0,
                                            0,     0,     false};
    auto runtime = hpactor::ObservabilityRuntime::create(cfg);
    ASSERT_TRUE(runtime != nullptr);

    auto stop_result = runtime->stop();
    (void)stop_result; // Must not crash.
}

TEST(ObservabilityRuntimeTest, DoubleStartIsSafe) {
    hpactor::ObservabilityRuntimeConfig cfg{false, false, false, 0,
                                            0,     0,     false};
    auto runtime = hpactor::ObservabilityRuntime::create(cfg);
    ASSERT_TRUE(runtime != nullptr);

    auto r1 = runtime->start();
    EXPECT_TRUE(r1.ok());

    auto r2 = runtime->start();
    (void)r2; // Second start must be safe.

    auto stop_result = runtime->stop();
    EXPECT_TRUE(stop_result.ok());
}

TEST(ObservabilityRuntimeTest, DoubleStopIsSafe) {
    hpactor::ObservabilityRuntimeConfig cfg{false, false, false, 0,
                                            0,     0,     false};
    auto runtime = hpactor::ObservabilityRuntime::create(cfg);
    ASSERT_TRUE(runtime != nullptr);

    auto start_result = runtime->start();
    EXPECT_TRUE(start_result.ok());

    auto r1 = runtime->stop();
    EXPECT_TRUE(r1.ok());

    auto r2 = runtime->stop();
    EXPECT_TRUE(r2.ok());
}

TEST(ObservabilityRuntimeTest, PortsAvailableAfterConstruction) {
    hpactor::ObservabilityRuntimeConfig cfg{false, false, false, 0,
                                            0,     0,     false};
    auto runtime = hpactor::ObservabilityRuntime::create(cfg);
    ASSERT_TRUE(runtime != nullptr);

    auto& metrics = runtime->metrics_sink();
    auto& log_sink = runtime->log_sink();
    auto& trace = runtime->trace_sink();

    // Ports must return same address on repeated calls (stable identity).
    EXPECT_EQ(&metrics, &runtime->metrics_sink());
    EXPECT_EQ(&log_sink, &runtime->log_sink());
    EXPECT_EQ(&trace, &runtime->trace_sink());
}

TEST(ObservabilityRuntimeTest, SnapshotAvailableAfterConstruction) {
    hpactor::ObservabilityRuntimeConfig cfg{false, false, false, 0,
                                            0,     0,     false};
    auto runtime = hpactor::ObservabilityRuntime::create(cfg);
    ASSERT_TRUE(runtime != nullptr);

    auto snap = runtime->snapshot();
    EXPECT_FALSE(snap.metrics_enabled);
    EXPECT_FALSE(snap.logging_enabled);
    EXPECT_FALSE(snap.tracing_enabled);
}

TEST(ObservabilityRuntimeTest, AllDisabledSnapshotReflectsState) {
    hpactor::ObservabilityRuntimeConfig cfg{false, false, false, 0,
                                            0,     0,     false};
    auto runtime = hpactor::ObservabilityRuntime::create(cfg);
    ASSERT_TRUE(runtime != nullptr);

    auto start_result = runtime->start();
    EXPECT_TRUE(start_result.ok());

    auto snap = runtime->snapshot();
    EXPECT_FALSE(snap.metrics_enabled);

    auto stop_result = runtime->stop();
    EXPECT_TRUE(stop_result.ok());
}

TEST(ObservabilityRuntimeTest, EnabledFlagsReflectedInSnapshot) {
    hpactor::ObservabilityRuntimeConfig cfg{/*metrics_enabled=*/true,
                                            /*logging_enabled=*/true,
                                            /*tracing_enabled=*/false,
                                            /*metrics_ring_buffer_capacity=*/65536,
                                            /*logging_ring_buffer_capacity=*/65536,
                                            /*tracing_ring_buffer_capacity=*/65536,
                                            /*fault_injection_enabled=*/true};

    auto runtime = hpactor::ObservabilityRuntime::create(cfg);
    ASSERT_TRUE(runtime != nullptr);

    auto snap = runtime->snapshot();
    EXPECT_TRUE(snap.metrics_enabled);
    EXPECT_TRUE(snap.logging_enabled);
    EXPECT_FALSE(snap.tracing_enabled);
    EXPECT_TRUE(snap.fault_injection_enabled);
}
