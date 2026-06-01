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

/// \brief Metric kind classification.
enum class MetricType {
    kCounter,   ///< Monotonically increasing cumulative value.
    kGauge,     ///< Point-in-time value that can go up or down.
    kHistogram, ///< Distribution of observations across buckets.
};

/// \brief Atomic counter value. 64-byte aligned to prevent false sharing.
struct alignas(64) CounterValue {
    std::atomic<uint64_t> total{0};
};

/// \brief Atomic gauge value. 64-byte aligned to prevent false sharing.
struct alignas(64) GaugeValue {
    std::atomic<int64_t> value{0};
};

/// \brief Number of histogram buckets for latency observation.
static constexpr size_t kHistogramNumBuckets = 16;

/// \brief Histogram bucket boundaries in seconds.
static constexpr double kBucketBoundsSec[kHistogramNumBuckets - 1] = {
    0.001, 0.002, 0.004, 0.008, 0.016, 0.032, 0.064, 0.128,
    0.256, 0.512, 1.024, 2.048, 4.096, 8.192, 16.384};

/// \brief Atomic histogram value with configurable buckets.
///
/// Records observations in nanoseconds. 64-byte aligned to prevent
/// false sharing.
struct alignas(64) HistogramValue {
    /// \brief Total number of observations.
    std::atomic<uint64_t> count{0};
    /// \brief Sum of all observed values in nanoseconds.
    std::atomic<uint64_t> sum_ns{0};
    /// \brief Per-bucket observation counts.
    std::atomic<uint64_t> buckets[kHistogramNumBuckets]{};

    /// \brief Record an observation.
    ///
    /// \param[in] value_ns Observed value in nanoseconds.
    void observe(uint64_t value_ns) noexcept;
};

/// \brief A set of labels identifying a unique metric time series.
struct LabelSet {
    /// \brief Prometheus-compatible metric name.
    std::string metric_name;
    /// \brief Ordered label key-value pairs.
    std::vector<std::pair<std::string, std::string>> labels;

    bool operator==(const LabelSet& other) const;
};

/// \brief Hash functor for LabelSet.
struct LabelSetHash {
    size_t operator()(const LabelSet& ls) const;
};

/// \brief A metric family grouping time series with the same name and type.
struct MetricFamily {
    /// \brief Metric name (Prometheus-compatible).
    std::string name;
    /// \brief HELP text for the metric.
    std::string help;
    /// \brief Metric type.
    MetricType type;
    /// \brief Label sets mapped to their values.
    std::unordered_map<LabelSet, std::variant<CounterValue, GaugeValue, HistogramValue>, LabelSetHash>
        values;
};

/// \brief Thread-safe registry for metric families and their time series.
///
/// Owns all metric families. Provides atomic read/write access to
/// individual metric values and atomic snapshot for export.
///
/// \note Thread safety: register_family() and get_or_create() use internal
///       synchronization. Counter/Gauge/Histogram updates are lock-free
///       via std::atomic.
class MetricRegistry {
  public:
    /// \brief Register or retrieve a metric family.
    ///
    /// If a family with the given name already exists, returns the existing
    /// family (ignoring \p help and \p type). Otherwise creates a new one.
    ///
    /// \param[in] name Metric name.
    /// \param[in] help HELP description string.
    /// \param[in] type Metric type.
    /// \return Reference to the (new or existing) metric family.
    MetricFamily&
    register_family(std::string name, std::string help, MetricType type);

    /// \brief Get or create a time series value within a family.
    ///
    /// \tparam V Value type (CounterValue, GaugeValue, or HistogramValue).
    /// \param[in] family The metric family.
    /// \param[in] labels Label set identifying the time series.
    /// \return Reference to the value.
    template <typename V>
    V& get_or_create(MetricFamily& family, const LabelSet& labels);

    /// \brief Atomic snapshot of all metric state.
    struct Snapshot {
        /// \brief A single histogram entry in the snapshot.
        struct HistogramEntry {
            LabelSet labels;
            uint64_t count;
            double sum_seconds;
            uint64_t buckets[kHistogramNumBuckets];
        };

        /// \brief Snapshot of a single metric family.
        struct FamilySnapshot {
            std::string name;
            std::string help;
            MetricType type;
            std::vector<std::pair<LabelSet, uint64_t>> counters;
            std::vector<std::pair<LabelSet, int64_t>> gauges;
            std::vector<HistogramEntry> histograms;
        };

        /// \brief Snapshot data for all registered families.
        std::vector<FamilySnapshot> families;
    };

    /// \brief Take an atomic snapshot of all registered metrics.
    ///
    /// Drains all atomic values into a consistent, non-atomic snapshot
    /// suitable for formatting and export.
    ///
    /// \return A consistent snapshot of all metric families.
    Snapshot snapshot() const;

  private:
    std::vector<std::unique_ptr<MetricFamily>> families_;
    std::unordered_map<std::string, MetricFamily*> family_index_;
};

} // namespace hpactor::metrics
