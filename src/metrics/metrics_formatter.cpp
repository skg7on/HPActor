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

#include <hpactor/metrics/metrics_formatter.hpp>
#include <cstdio>

namespace hpactor::metrics {

std::string OpenMetricsFormatter::escape_label_value(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        default:   out += c;
        }
    }
    return out;
}

std::string OpenMetricsFormatter::format_labels(const LabelSet& ls) {
    if (ls.labels.empty()) return "";
    std::string out = "{";
    for (size_t i = 0; i < ls.labels.size(); ++i) {
        if (i > 0) out += ",";
        out += ls.labels[i].first;
        out += "=\"";
        out += escape_label_value(ls.labels[i].second);
        out += "\"";
    }
    out += "}";
    return out;
}

std::string OpenMetricsFormatter::format(const MetricRegistry::Snapshot& snapshot) const {
    std::string out;
    out.reserve(8192);

    char buf[256];

    for (const auto& fam : snapshot.families) {
        out += "# HELP ";
        out += fam.name;
        out += " ";
        out += fam.help;
        out += "\n";

        switch (fam.type) {
        case MetricType::kCounter: {
            out += "# TYPE ";
            out += fam.name;
            out += " counter\n";
            for (const auto& [ls, val] : fam.counters) {
                int n = snprintf(buf, sizeof(buf), "%s%s %llu\n",
                                 fam.name.c_str(), format_labels(ls).c_str(),
                                 static_cast<unsigned long long>(val));
                out.append(buf, static_cast<size_t>(n));
            }
            break;
        }
        case MetricType::kGauge: {
            out += "# TYPE ";
            out += fam.name;
            out += " gauge\n";
            for (const auto& [ls, val] : fam.gauges) {
                int n = snprintf(buf, sizeof(buf), "%s%s %lld\n",
                                 fam.name.c_str(), format_labels(ls).c_str(),
                                 static_cast<long long>(val));
                out.append(buf, static_cast<size_t>(n));
            }
            break;
        }
        case MetricType::kHistogram: {
            out += "# TYPE ";
            out += fam.name;
            out += " histogram\n";
            for (const auto& he : fam.histograms) {
                std::string base_label_str = format_labels(he.labels);
                uint64_t cumulative = 0;
                for (size_t i = 0; i < kHistogramNumBuckets - 1; ++i) {
                    cumulative += he.buckets[i];
                    std::string extra_label;
                    if (he.labels.labels.empty()) {
                        extra_label = "{le=\"";
                    } else {
                        extra_label = base_label_str;
                        extra_label.pop_back();  // remove trailing }
                        extra_label += ",le=\"";
                    }
                    int n = snprintf(buf, sizeof(buf),
                                     "%s_bucket%s%g\"} %llu\n",
                                     fam.name.c_str(), extra_label.c_str(),
                                     kBucketBounds[i],
                                     static_cast<unsigned long long>(cumulative));
                    out.append(buf, static_cast<size_t>(n));
                }
                cumulative += he.buckets[kHistogramNumBuckets - 1];
                {
                    std::string extra_label;
                    if (he.labels.labels.empty()) {
                        extra_label = "{le=\"+Inf\"}";
                    } else {
                        extra_label = base_label_str;
                        extra_label.pop_back();
                        extra_label += ",le=\"+Inf\"}";
                    }
                    int n = snprintf(buf, sizeof(buf),
                                     "%s_bucket%s %llu\n",
                                     fam.name.c_str(), extra_label.c_str(),
                                     static_cast<unsigned long long>(cumulative));
                    out.append(buf, static_cast<size_t>(n));
                }
                {
                    int n = snprintf(buf, sizeof(buf), "%s_sum%s %g\n",
                                     fam.name.c_str(), base_label_str.c_str(),
                                     he.sum_seconds);
                    out.append(buf, static_cast<size_t>(n));
                }
                {
                    int n = snprintf(buf, sizeof(buf), "%s_count%s %llu\n",
                                     fam.name.c_str(), base_label_str.c_str(),
                                     static_cast<unsigned long long>(he.count));
                    out.append(buf, static_cast<size_t>(n));
                }
            }
            break;
        }
        }
        out += "\n";
    }
    out += "# EOF\n";
    return out;
}

} // namespace hpactor::metrics
