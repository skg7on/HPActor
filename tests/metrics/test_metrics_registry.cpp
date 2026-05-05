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

#include <hpactor/metrics/metrics_registry.hpp>
#include <cassert>
#include <cstdio>

int main() {
    using namespace hpactor::metrics;

    // Test Counter
    MetricRegistry reg;
    auto& family = reg.register_family("test_counter", "help", MetricType::kCounter);
    CounterValue& c = reg.get_or_create<CounterValue>(family, LabelSet{});
    c.total.fetch_add(5, std::memory_order_relaxed);
    auto snap = reg.snapshot();
    assert(snap.families.size() == 1);
    assert(snap.families[0].counters.size() == 1);
    assert(snap.families[0].counters[0].second == 5);

    // Test Gauge
    auto& gfamily = reg.register_family("test_gauge", "help", MetricType::kGauge);
    GaugeValue& g = reg.get_or_create<GaugeValue>(gfamily, LabelSet{});
    g.value.fetch_add(3, std::memory_order_relaxed);
    g.value.fetch_sub(1, std::memory_order_relaxed);
    snap = reg.snapshot();
    for (auto& fs : snap.families) {
        if (fs.name == "test_gauge") {
            assert(fs.gauges[0].second == 2);
        }
    }

    // Test Histogram
    auto& hfamily = reg.register_family("test_hist", "help", MetricType::kHistogram);
    HistogramValue& h = reg.get_or_create<HistogramValue>(hfamily, LabelSet{});
    h.observe(5'000'000);  // 5ms
    snap = reg.snapshot();
    for (auto& fs : snap.families) {
        if (fs.name == "test_hist") {
            assert(fs.histograms[0].count == 1);
            assert(fs.histograms[0].sum_seconds > 0.0);
        }
    }

    printf("test_metrics_registry: PASSED\n");
    return 0;
}
