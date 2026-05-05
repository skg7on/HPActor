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
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/metrics/metrics_registry.hpp>
#include <hpactor/metrics/metrics_formatter.hpp>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>

using namespace hpactor::metrics;

int main() {
    // Test the end-to-end pipeline: ring buffer → registry → formatter

    MpscRingBuffer<MetricEvent> ring_buf;
    MetricRegistry registry;

    // Register families manually (same as Aggregator would do)
    auto& depth_fam = registry.register_family(
        "hpactor_mailbox_depth", "Current mailbox queue depth.", MetricType::kGauge);
    auto& msg_fam = registry.register_family(
        "hpactor_mailbox_messages_total", "Total messages enqueued.", MetricType::kCounter);
    auto& latency_fam = registry.register_family(
        "hpactor_message_processing_seconds", "Message processing latency.",
        MetricType::kHistogram);

    // Write events to ring buffer
    for (int i = 0; i < 3; ++i) {
        MetricEvent evt{};
        evt.actor_id = hpactor::ActorId(1);
        evt.event_type = MetricEventType::kMailboxEnqueue;
        evt.value_hi = 1;
        ring_buf.try_push(evt);
    }
    {
        MetricEvent evt{};
        evt.actor_id = hpactor::ActorId(1);
        evt.event_type = MetricEventType::kMailboxDequeue;
        evt.value_hi = 1;
        ring_buf.try_push(evt);
    }
    {
        MetricEvent evt{};
        evt.actor_id = hpactor::ActorId(1);
        evt.event_type = MetricEventType::kMessageProcessed;
        evt.value_hi = 5'000'000;  // 5ms
        ring_buf.try_push(evt);
    }

    // Drain ring buffer and aggregate into registry
    LabelSet actor_ls;
    actor_ls.labels.emplace_back("actor_id", "1");
    actor_ls.labels.emplace_back("actor_type", "TestActor");

    GaugeValue* depth_g = nullptr;
    CounterValue* msg_c = nullptr;
    HistogramValue* lat_h = nullptr;

    ring_buf.drain([&](const MetricEvent& e) {
        switch (e.event_type) {
        case MetricEventType::kMailboxEnqueue: {
            if (!depth_g) depth_g = &registry.get_or_create<GaugeValue>(depth_fam, actor_ls);
            depth_g->value.fetch_add(1, std::memory_order_relaxed);
            if (!msg_c) msg_c = &registry.get_or_create<CounterValue>(msg_fam, actor_ls);
            msg_c->total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kMailboxDequeue: {
            if (!depth_g) depth_g = &registry.get_or_create<GaugeValue>(depth_fam, actor_ls);
            depth_g->value.fetch_sub(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kMessageProcessed: {
            if (!lat_h) lat_h = &registry.get_or_create<HistogramValue>(latency_fam, actor_ls);
            lat_h->observe(e.value_hi);
            break;
        }
        default:
            break;
        }
        return true;
    });

    // Verify aggregated values
    {
        auto snap = registry.snapshot();
        for (const auto& fs : snap.families) {
            if (fs.name == "hpactor_mailbox_depth") {
                assert(!fs.gauges.empty());
                assert(fs.gauges[0].second == 2);  // 3 enqueue - 1 dequeue
            }
            if (fs.name == "hpactor_mailbox_messages_total") {
                assert(!fs.counters.empty());
                assert(fs.counters[0].second == 3);
            }
            if (fs.name == "hpactor_message_processing_seconds") {
                assert(!fs.histograms.empty());
                assert(fs.histograms[0].count == 1);
                assert(fs.histograms[0].sum_seconds > 0.0);
            }
        }
    }

    // Format as OpenMetrics and verify output
    OpenMetricsFormatter fmt;
    auto snap = registry.snapshot();
    std::string text = fmt.format(snap);

    assert(!text.empty());
    assert(text.find("# HELP hpactor_mailbox_depth") != std::string::npos);
    assert(text.find("# TYPE hpactor_mailbox_depth gauge") != std::string::npos);
    assert(text.find("# HELP hpactor_mailbox_messages_total") != std::string::npos);
    assert(text.find("# TYPE hpactor_mailbox_messages_total counter") != std::string::npos);
    assert(text.find("# HELP hpactor_message_processing_seconds") != std::string::npos);
    assert(text.find("# TYPE hpactor_message_processing_seconds histogram") != std::string::npos);
    assert(text.find("_bucket{") != std::string::npos);
    assert(text.find("_sum{") != std::string::npos);
    assert(text.find("_count{") != std::string::npos);
    assert(text.find("# EOF") != std::string::npos);

    // Test multi-producer ring buffer
    MpscRingBuffer<MetricEvent> mp_buf;
    std::atomic<size_t> total{0};

    auto producer = [&]() {
        for (int i = 0; i < 1000; ++i) {
            MetricEvent evt{};
            evt.actor_id = hpactor::ActorId(1);
            evt.event_type = MetricEventType::kMailboxEnqueue;
            evt.value_hi = 1;
            while (!mp_buf.try_push(evt)) {}
        }
    };

    std::thread t1(producer);
    std::thread t2(producer);
    std::thread t3(producer);
    t1.join();
    t2.join();
    t3.join();

    mp_buf.drain([&](const MetricEvent&) {
        total.fetch_add(1, std::memory_order_relaxed);
        return true;
    });

    assert(total.load() == 3000);
    assert(mp_buf.empty());

    printf("test_metrics_integration: PASSED\n");
    return 0;
}
