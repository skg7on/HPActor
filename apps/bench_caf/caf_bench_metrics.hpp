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

} // namespace hpactor::apps::bench_caf
