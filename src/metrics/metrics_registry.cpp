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

#include <algorithm>
#include <tuple>

namespace hpactor::metrics {

void HistogramValue::observe(uint64_t value_ns) noexcept {
    uint64_t ms = value_ns / 1'000'000;
    int idx = 0;
    if (ms > 1) {
        idx = 64 - __builtin_clzll(ms - 1);
        idx = std::max(0, idx);
    }
    if (idx >= static_cast<int>(kHistogramNumBuckets)) {
        idx = kHistogramNumBuckets - 1;
    }
    buckets[idx].fetch_add(1, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
    sum_ns.fetch_add(value_ns, std::memory_order_relaxed);
}

bool LabelSet::operator==(const LabelSet& other) const {
    if (metric_name != other.metric_name) return false;
    return labels == other.labels;
}

size_t LabelSetHash::operator()(const LabelSet& ls) const {
    size_t h = std::hash<std::string>()(ls.metric_name);
    for (const auto& [k, v] : ls.labels) {
        h ^= std::hash<std::string>()(k) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>()(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
}

MetricFamily& MetricRegistry::register_family(std::string name, std::string help,
                                                MetricType type) {
    auto it = family_index_.find(name);
    if (it != family_index_.end()) {
        return *it->second;
    }
    auto family = std::make_unique<MetricFamily>();
    family->name = name;
    family->help = std::move(help);
    family->type = type;
    MetricFamily* ptr = family.get();
    family_index_[name] = ptr;
    families_.push_back(std::move(family));
    return *ptr;
}

template <typename V>
V& MetricRegistry::get_or_create(MetricFamily& family, const LabelSet& labels) {
    auto it = family.values.find(labels);
    if (it != family.values.end()) {
        return std::get<V>(it->second);
    }
    auto [inserted, _] = family.values.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(labels),
        std::forward_as_tuple(std::in_place_type<V>));
    return std::get<V>(inserted->second);
}

template CounterValue& MetricRegistry::get_or_create<CounterValue>(MetricFamily&, const LabelSet&);
template GaugeValue& MetricRegistry::get_or_create<GaugeValue>(MetricFamily&, const LabelSet&);
template HistogramValue& MetricRegistry::get_or_create<HistogramValue>(MetricFamily&, const LabelSet&);

MetricRegistry::Snapshot MetricRegistry::snapshot() const {
    Snapshot snap;
    for (const auto& family : families_) {
        Snapshot::FamilySnapshot fs;
        fs.name = family->name;
        fs.help = family->help;
        fs.type = family->type;

        for (const auto& [ls, val] : family->values) {
            switch (family->type) {
            case MetricType::kCounter: {
                uint64_t v = std::get<CounterValue>(val).total.load(std::memory_order_relaxed);
                fs.counters.emplace_back(ls, v);
                break;
            }
            case MetricType::kGauge: {
                int64_t v = std::get<GaugeValue>(val).value.load(std::memory_order_relaxed);
                fs.gauges.emplace_back(ls, v);
                break;
            }
            case MetricType::kHistogram: {
                const auto& hv = std::get<HistogramValue>(val);
                Snapshot::HistogramEntry he;
                he.labels = ls;
                he.count = hv.count.load(std::memory_order_relaxed);
                he.sum_seconds = static_cast<double>(hv.sum_ns.load(std::memory_order_relaxed)) / 1e9;
                for (size_t i = 0; i < kHistogramNumBuckets; ++i) {
                    he.buckets[i] = hv.buckets[i].load(std::memory_order_relaxed);
                }
                fs.histograms.push_back(std::move(he));
                break;
            }
            }
        }
        snap.families.push_back(std::move(fs));
    }
    return snap;
}

} // namespace hpactor::metrics
