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

#include <hpactor/runtime/observability_ports.hpp>
#include <hpactor/tracing/span.hpp> // for SpanHandle

TEST(MetricsSinkPortTest, DefaultConstructedIsNoop) {
    hpactor::MetricsSinkPort port;
    EXPECT_EQ(port.context, nullptr);
    EXPECT_EQ(port.emit, nullptr);
}

TEST(MetricsSinkPortTest, WiredPortDeliversEvent) {
    std::atomic<uint32_t> event_count{0};

    hpactor::MetricsSinkPort port;
    port.context = &event_count;
    port.emit = [](void* ctx, uint32_t /*event_type*/, uint64_t /*val*/) noexcept {
        auto* c = static_cast<std::atomic<uint32_t>*>(ctx);
        c->fetch_add(1);
    };

    EXPECT_NE(port.emit, nullptr);
    port.emit(&event_count, 1, 42);
    EXPECT_EQ(event_count.load(), 1u);
}

TEST(MetricsSinkPortTest, PortIdentityIsStable) {
    hpactor::MetricsSinkPort port;
    const auto* addr = &port;
    EXPECT_EQ(addr, &port);
}

// ── LogSinkPort ────────────────────────────────────────────────────────────

TEST(LogSinkPortTest, DefaultConstructedIsNoop) {
    hpactor::LogSinkPort port;
    EXPECT_EQ(port.context, nullptr);
    EXPECT_EQ(port.emit, nullptr);
}

TEST(LogSinkPortTest, WiredPortHasEmitCallback) {
    std::atomic<uint32_t> log_count{0};

    hpactor::LogSinkPort port;
    port.context = &log_count;
    port.emit = [](void* ctx, const hpactor::log::LogEvent& /*event*/,
                   bool /*high_priority*/) noexcept {
        auto* c = static_cast<std::atomic<uint32_t>*>(ctx);
        c->fetch_add(1);
    };

    EXPECT_NE(port.emit, nullptr);
}

TEST(LogSinkPortTest, PortIdentityIsStable) {
    hpactor::LogSinkPort port;
    const auto* addr = &port;
    EXPECT_EQ(addr, &port);
}

// ── TraceSinkPort ──────────────────────────────────────────────────────────

TEST(TraceSinkPortTest, DefaultConstructedIsNoop) {
    hpactor::TraceSinkPort port;
    EXPECT_EQ(port.context, nullptr);
    EXPECT_EQ(port.record_span, nullptr);
}

TEST(TraceSinkPortTest, WiredPortHasRecordSpanCallback) {
    hpactor::TraceSinkPort port;
    port.record_span = [](void* /*ctx*/,
                          const hpactor::tracing::SpanStart& /*span*/) noexcept {
        return hpactor::tracing::SpanHandle{};
    };
    EXPECT_NE(port.record_span, nullptr);
}

TEST(TraceSinkPortTest, PortIdentityIsStable) {
    hpactor::TraceSinkPort port;
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
