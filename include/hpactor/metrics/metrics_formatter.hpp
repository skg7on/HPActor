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

#include <hpactor/metrics/metrics_registry.hpp>
#include <string>

namespace hpactor::metrics {

/// \brief Prometheus OpenMetrics text format formatter.
///
/// Converts a MetricRegistry::Snapshot into the Prometheus exposition
/// format (text/plain; version=1.0.0) with HELP, TYPE, metric lines,
/// and # EOF.
class OpenMetricsFormatter {
  public:
    /// \brief Format a metric snapshot as a Prometheus exposition string.
    ///
    /// \param[in] snapshot Atomic snapshot from MetricRegistry::snapshot().
    /// \return A complete Prometheus text-format response including HELP,
    ///         TYPE, metric values, and # EOF.
    std::string format(const MetricRegistry::Snapshot& snapshot) const;

  private:
    /// \brief Format a LabelSet as Prometheus label string.
    ///
    /// \param[in] ls The label set.
    /// \return Formatted label string, e.g. \c {key1="val1",key2="val2"}.
    static std::string format_labels(const LabelSet& ls);

    /// \brief Escape special characters in a label value.
    ///
    /// \param[in] s Raw label value.
    /// \return Escaped string safe for Prometheus output.
    static std::string escape_label_value(const std::string& s);

    /// \brief Histogram bucket boundaries in seconds.
    static constexpr double kBucketBounds[15] = {
        0.001, 0.002, 0.004, 0.008, 0.016, 0.032, 0.064, 0.128,
        0.256, 0.512, 1.024, 2.048, 4.096, 8.192, 16.384};
};

} // namespace hpactor::metrics
