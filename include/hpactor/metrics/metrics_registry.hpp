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

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace hpactor::metrics {

enum class MetricType { kCounter, kGauge, kHistogram };

struct alignas(64) CounterValue {
    std::atomic<uint64_t> total{0};
};

struct alignas(64) GaugeValue {
    std::atomic<int64_t> value{0};
};

static constexpr size_t kHistogramNumBuckets = 16;
static constexpr double kBucketBoundsSec[kHistogramNumBuckets - 1] = {
    0.001, 0.002, 0.004, 0.008, 0.016, 0.032, 0.064,
    0.128, 0.256, 0.512, 1.024, 2.048, 4.096, 8.192, 16.384
};

struct alignas(64) HistogramValue {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> sum_ns{0};
    std::atomic<uint64_t> buckets[kHistogramNumBuckets]{};

    void observe(uint64_t value_ns) noexcept;
};

struct LabelSet {
    std::string metric_name;
    std::vector<std::pair<std::string, std::string>> labels;

    bool operator==(const LabelSet& other) const;
};

struct LabelSetHash {
    size_t operator()(const LabelSet& ls) const;
};

struct MetricFamily {
    std::string name;
    std::string help;
    MetricType  type;
    std::unordered_map<LabelSet,
        std::variant<CounterValue, GaugeValue, HistogramValue>,
        LabelSetHash> values;
};

class MetricRegistry {
public:
    MetricFamily& register_family(std::string name, std::string help, MetricType type);

    template <typename V>
    V& get_or_create(MetricFamily& family, const LabelSet& labels);

    struct Snapshot {
        struct HistogramEntry {
            LabelSet labels;
            uint64_t count;
            double   sum_seconds;
            uint64_t buckets[kHistogramNumBuckets];
        };
        struct FamilySnapshot {
            std::string name;
            std::string help;
            MetricType type;
            std::vector<std::pair<LabelSet, uint64_t>> counters;
            std::vector<std::pair<LabelSet, int64_t>> gauges;
            std::vector<HistogramEntry> histograms;
        };
        std::vector<FamilySnapshot> families;
    };

    Snapshot snapshot() const;

private:
    std::vector<std::unique_ptr<MetricFamily>> families_;
    std::unordered_map<std::string, MetricFamily*> family_index_;
};

} // namespace hpactor::metrics
