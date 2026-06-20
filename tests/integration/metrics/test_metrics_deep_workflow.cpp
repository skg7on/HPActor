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

#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_formatter.hpp>
#include <hpactor/metrics/metrics_registry.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace hpactor::metrics;

// ── MetricRegistry: register_family idempotency ──────────────────

TEST(MetricsDeepWorkflowTest, RegisterFamilyIdempotentSameName) {
    MetricRegistry reg;
    auto& f1 = reg.register_family("my_metric", "first help", MetricType::kCounter);
    EXPECT_EQ(f1.name, "my_metric");
    EXPECT_EQ(f1.help, "first help");
    EXPECT_EQ(f1.type, MetricType::kCounter);

    // Second registration with same name but different help/type: returns
    // the existing family unchanged.
    auto& f2 = reg.register_family("my_metric", "second help", MetricType::kGauge);
    EXPECT_EQ(&f1, &f2);
    EXPECT_EQ(f2.help, "first help");
    EXPECT_EQ(f2.type, MetricType::kCounter);
}

TEST(MetricsDeepWorkflowTest, RegisterFamilyMultipleDistinct) {
    MetricRegistry reg;
    auto& c = reg.register_family("counter_a", "h", MetricType::kCounter);
    auto& g = reg.register_family("gauge_b", "h", MetricType::kGauge);
    auto& h = reg.register_family("histogram_c", "h", MetricType::kHistogram);
    EXPECT_NE(&c, &g);
    EXPECT_NE(&g, &h);

    auto snap = reg.snapshot();
    EXPECT_EQ(snap.families.size(), 3u);
}

// ── MetricRegistry: get_or_create with labels ─────────────────────

TEST(MetricsDeepWorkflowTest, GetOrCreateSameLabelsReturnsExisting) {
    MetricRegistry reg;
    auto& fam = reg.register_family("counter", "help", MetricType::kCounter);

    LabelSet ls;
    ls.labels.emplace_back("actor", "1");

    CounterValue& c1 = reg.get_or_create<CounterValue>(fam, ls);
    c1.total.fetch_add(100, std::memory_order_relaxed);

    CounterValue& c2 = reg.get_or_create<CounterValue>(fam, ls);
    EXPECT_EQ(&c1, &c2);
    EXPECT_EQ(c2.total.load(std::memory_order_relaxed), 100u);
}

TEST(MetricsDeepWorkflowTest, GetOrCreateDifferentLabelsCreatesNew) {
    MetricRegistry reg;
    auto& fam = reg.register_family("counter", "help", MetricType::kCounter);

    LabelSet ls1;
    ls1.labels.emplace_back("actor", "1");
    CounterValue& c1 = reg.get_or_create<CounterValue>(fam, ls1);
    c1.total.fetch_add(10, std::memory_order_relaxed);

    LabelSet ls2;
    ls2.labels.emplace_back("actor", "2");
    CounterValue& c2 = reg.get_or_create<CounterValue>(fam, ls2);
    c2.total.fetch_add(20, std::memory_order_relaxed);

    EXPECT_NE(&c1, &c2);

    auto snap = reg.snapshot();
    EXPECT_EQ(snap.families.size(), 1u);
    EXPECT_EQ(snap.families[0].counters.size(), 2u);
}

// ── MetricRegistry: empty snapshot ────────────────────────────────

TEST(MetricsDeepWorkflowTest, EmptyRegistrySnapshot) {
    MetricRegistry reg;
    auto snap = reg.snapshot();
    EXPECT_TRUE(snap.families.empty());
}

// ── LabelSet equality and hashing ─────────────────────────────────

TEST(MetricsDeepWorkflowTest, LabelSetEquality) {
    LabelSet a;
    a.metric_name = "test";
    a.labels = {{"k", "v"}};

    LabelSet b;
    b.metric_name = "test";
    b.labels = {{"k", "v"}};

    LabelSet c;
    c.metric_name = "test";
    c.labels = {{"k", "w"}};

    LabelSet d;
    d.metric_name = "other";
    d.labels = {{"k", "v"}};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
}

TEST(MetricsDeepWorkflowTest, LabelSetHash) {
    LabelSet a;
    a.metric_name = "metric_a";
    a.labels = {{"x", "1"}};

    LabelSet b;
    b.metric_name = "metric_a";
    b.labels = {{"x", "1"}};

    LabelSetHash hasher;
    EXPECT_EQ(hasher(a), hasher(b));
}

// ── HistogramValue bucket boundaries ──────────────────────────────

TEST(MetricsDeepWorkflowTest, HistogramObserveBoundaries) {
    // Verify observations land in the correct buckets.
    // Bucket boundaries (seconds): 0.001, 0.002, 0.004, 0.008, 0.016,
    // 0.032, 0.064, 0.128, 0.256, 0.512, 1.024, 2.048, 4.096, 8.192, 16.384
    HistogramValue hv;

    // 1ms → bucket 0 (the ≤ 0.001s bucket)
    hv.observe(1'000'000); // 1ms = 1,000,000 ns
    EXPECT_EQ(hv.buckets[0].load(std::memory_order_relaxed), 1u);

    // 5ms = 5,000,000 ns → check it's in a higher bucket
    hv.observe(5'000'000);
    EXPECT_EQ(hv.count.load(std::memory_order_relaxed), 2u);

    // Very large value → last bucket
    hv.observe(60'000'000'000ULL); // 60 seconds
    EXPECT_EQ(hv.buckets[15].load(std::memory_order_relaxed), 1u);
}

TEST(MetricsDeepWorkflowTest, HistogramObserveZeroValue) {
    HistogramValue hv;
    hv.observe(0);
    EXPECT_EQ(hv.count.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(hv.sum_ns.load(std::memory_order_relaxed), 0u);
    // 0ms → value 0 enters the first bucket (ms = 0, the `ms > 1` check is
    // false, so `idx` stays 0).
    EXPECT_EQ(hv.buckets[0].load(std::memory_order_relaxed), 1u);
}

// ── OpenMetricsFormatter edge cases ───────────────────────────────

TEST(MetricsDeepWorkflowTest, FormatterEmptySnapshot) {
    OpenMetricsFormatter fmt;
    MetricRegistry::Snapshot snap;
    std::string text = fmt.format(snap);
    // Should produce at least "# EOF\n".
    EXPECT_NE(text.find("# EOF"), std::string::npos);
}

TEST(MetricsDeepWorkflowTest, FormatterEscapesLabelValues) {
    MetricRegistry reg;
    auto& fam = reg.register_family("test", "help", MetricType::kCounter);

    LabelSet ls;
    ls.labels.emplace_back("path", "C:\\dir\\file");
    ls.labels.emplace_back("desc", "say \"hello\"");
    auto& c = reg.get_or_create<CounterValue>(fam, ls);
    c.total.fetch_add(1, std::memory_order_relaxed);

    auto snap = reg.snapshot();
    OpenMetricsFormatter fmt;
    std::string text = fmt.format(snap);

    // Escaped backslash and quote are both present
    EXPECT_NE(text.find("\\\\"), std::string::npos);
    EXPECT_NE(text.find("\\\""), std::string::npos);
    EXPECT_NE(text.find("\\\""), std::string::npos);
}

TEST(MetricsDeepWorkflowTest, FormatterHandlesNegativeGauge) {
    MetricRegistry reg;
    auto& fam = reg.register_family("temp", "temperature", MetricType::kGauge);
    LabelSet ls;
    GaugeValue& g = reg.get_or_create<GaugeValue>(fam, ls);
    g.value.store(-42, std::memory_order_relaxed);

    auto snap = reg.snapshot();
    OpenMetricsFormatter fmt;
    std::string text = fmt.format(snap);
    EXPECT_NE(text.find("-42"), std::string::npos);
}

TEST(MetricsDeepWorkflowTest, FormatterZeroCounterShowsValue) {
    MetricRegistry reg;
    auto& fam = reg.register_family("zero_ctr", "help", MetricType::kCounter);
    LabelSet ls;
    (void)reg.get_or_create<CounterValue>(fam, ls);

    auto snap = reg.snapshot();
    OpenMetricsFormatter fmt;
    std::string text = fmt.format(snap);

    // Should contain the metric name and a line with value (even if 0).
    EXPECT_NE(text.find("zero_ctr"), std::string::npos);
    EXPECT_NE(text.find(" 0"), std::string::npos);
}

// ── HistogramSnapshot sum_seconds calculation ─────────────────────

TEST(MetricsDeepWorkflowTest, HistogramSnapshotSumSeconds) {
    MetricRegistry reg;
    auto& fam = reg.register_family("latency", "help", MetricType::kHistogram);
    LabelSet ls;
    HistogramValue& h = reg.get_or_create<HistogramValue>(fam, ls);
    // Observe exactly 1 second (1,000,000,000 ns)
    h.observe(1'000'000'000);
    h.observe(2'000'000'000); // 2 seconds

    auto snap = reg.snapshot();
    ASSERT_EQ(snap.families.size(), 1u);
    ASSERT_EQ(snap.families[0].histograms.size(), 1u);

    // sum_seconds = 3e9 / 1e9 = 3.0
    EXPECT_DOUBLE_EQ(snap.families[0].histograms[0].sum_seconds, 3.0);
    EXPECT_EQ(snap.families[0].histograms[0].count, 2u);
}

// ── Multi-family, multi-timeseries snapshot ───────────────────────

TEST(MetricsDeepWorkflowTest, MultiFamilyMultiTimeseriesSnapshot) {
    MetricRegistry reg;

    // Counter family with two timeseries
    auto& c_fam = reg.register_family("req_total", "help", MetricType::kCounter);
    LabelSet ok_ls;
    ok_ls.labels.emplace_back("status", "ok");
    reg.get_or_create<CounterValue>(c_fam, ok_ls)
        .total.fetch_add(200, std::memory_order_relaxed);

    LabelSet err_ls;
    err_ls.labels.emplace_back("status", "error");
    reg.get_or_create<CounterValue>(c_fam, err_ls)
        .total.fetch_add(15, std::memory_order_relaxed);

    // Gauge family
    auto& g_fam = reg.register_family("active", "help", MetricType::kGauge);
    LabelSet gls;
    reg.get_or_create<GaugeValue>(g_fam, gls).value.store(7, std::memory_order_relaxed);

    auto snap = reg.snapshot();
    EXPECT_EQ(snap.families.size(), 2u);

    for (const auto& fs : snap.families) {
        if (fs.name == "req_total") {
            EXPECT_EQ(fs.counters.size(), 2u);
        }
        if (fs.name == "active") {
            EXPECT_EQ(fs.gauges.size(), 1u);
            EXPECT_EQ(fs.gauges[0].second, 7);
        }
    }
}

// ── Ring buffer edge cases ────────────────────────────────────────

TEST(MetricsDeepWorkflowTest, RingBufferNoEventsLostInitially) {
    MpscRingBuffer<MetricEvent> buf;
    EXPECT_EQ(buf.events_lost(), 0u);
    EXPECT_TRUE(buf.empty());
}

TEST(MetricsDeepWorkflowTest, RingBufferDrainEmptyReturnsZero) {
    MpscRingBuffer<MetricEvent> buf;
    size_t count = buf.drain([](const MetricEvent&) { return true; });
    EXPECT_EQ(count, 0u);
}

TEST(MetricsDeepWorkflowTest, RingBufferEventsLostTracking) {
    // Use a small capacity to force overflow.
    hpactor::adt::DynamicMpscRingBuffer<MetricEvent> buf(4);
    MetricEvent evt{};
    for (int i = 0; i < 6; ++i) {
        (void)buf.try_push(evt);
    }
    EXPECT_EQ(buf.events_lost(), 2u);
    EXPECT_EQ(buf.size(), 4u);
}

TEST(MetricsDeepWorkflowTest, RingBufferFifoOrdering) {
    MpscRingBuffer<MetricEvent> buf;
    for (uint32_t i = 0; i < 3; ++i) {
        MetricEvent evt{};
        evt.value_hi = i;
        buf.try_push(evt);
    }

    uint64_t expected = 0;
    buf.drain([&](const MetricEvent& e) {
        EXPECT_EQ(e.value_hi, expected);
        ++expected;
        return true;
    });
    EXPECT_EQ(expected, 3u);
}
