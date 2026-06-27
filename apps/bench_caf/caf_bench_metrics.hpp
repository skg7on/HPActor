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

#include "caf_bench_config.hpp"

#include <cstdint>
#include <vector>

namespace hpactor::apps::bench_caf {

inline constexpr size_t kHistogramBuckets = 8;

struct TrialMetrics {
    uint32_t trial = 0;
    bool completed = false;
    uint64_t runtime_ms = 0;
    uint64_t total_sent = 0;
    uint64_t total_received = 0;
    uint64_t total_rejected = 0;
    uint64_t total_dropped = 0;
    uint64_t actors_created = 0;
    uint64_t actors_completed = 0;
    uint64_t rings_completed = 0;
    uint64_t token_hops = 0;
    uint64_t cpu_tasks_completed = 0;
    double throughput_msgps = 0.0;
    uint64_t peak_rss_bytes = 0;
    std::vector<uint64_t> rss_samples_bytes;

    // Phase 2 fields
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    double bytes_per_second = 0.0;
    uint64_t admission_failures = 0;
    uint64_t dlq_handoffs = 0;
    uint64_t dedup_suppressions = 0;
    uint64_t deadline_expiries = 0;
    uint64_t max_receiver_depth = 0;
    uint64_t min_receiver_messages = 0;
    uint64_t max_receiver_messages = 0;
    double min_sender_throughput = 0.0;
    double max_sender_throughput = 0.0;
    std::vector<uint64_t> size_histogram;
};

struct CafBenchReport {
    std::string schema_version = "1";
    CafBenchConfig config;
    std::vector<TrialMetrics> trials;
};

inline uint64_t peak_rss(const std::vector<uint64_t>& samples) {
    uint64_t peak = 0;
    for (auto value : samples) {
        if (value > peak)
            peak = value;
    }
    return peak;
}

inline double throughput(uint64_t messages, uint64_t runtime_ms) {
    if (runtime_ms == 0)
        return 0.0;
    return static_cast<double>(messages) * 1000.0 / static_cast<double>(runtime_ms);
}

inline std::pair<uint64_t, uint64_t>
compute_receiver_skew(const std::vector<uint64_t>& counts) {
    if (counts.empty())
        return {0, 0};
    uint64_t min_v = UINT64_MAX;
    uint64_t max_v = 0;
    for (auto v : counts) {
        if (v < min_v)
            min_v = v;
        if (v > max_v)
            max_v = v;
    }
    return {min_v, max_v};
}

inline std::pair<double, double>
compute_sender_spread(const std::vector<double>& throughputs) {
    if (throughputs.empty())
        return {0.0, 0.0};
    double min_v = throughputs[0];
    double max_v = throughputs[0];
    for (auto v : throughputs) {
        if (v < min_v)
            min_v = v;
        if (v > max_v)
            max_v = v;
    }
    return {min_v, max_v};
}

inline std::vector<uint64_t>
build_size_histogram(const std::vector<size_t>& sizes) {
    std::vector<uint64_t> hist(kHistogramBuckets, 0);
    // Bucket boundaries: 0, 16, 64, 256, 1024, 4096, 16384, MAX
    constexpr size_t boundaries[] = {0,    16,   64,    256,
                                     1024, 4096, 16384, SIZE_MAX};
    for (auto sz : sizes) {
        for (size_t b = 0; b < kHistogramBuckets; ++b) {
            if (sz <= boundaries[b]) {
                ++hist[b];
                break;
            }
            if (b == kHistogramBuckets - 1)
                ++hist[b];
        }
    }
    return hist;
}

} // namespace hpactor::apps::bench_caf
