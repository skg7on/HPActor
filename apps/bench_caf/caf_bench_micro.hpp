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
#include "caf_bench_metrics.hpp"
#include "messages.hpp"

#include <chrono>
#include <cstdint>
#include <functional>

namespace hpactor::apps::bench_caf {

struct MicroResult {
    uint64_t iterations = 0;
    uint64_t runtime_ns = 0;
    uint64_t ops_per_sec = 0;
    uint64_t alloc_count = 0;
    uint64_t alloc_bytes = 0;
};

inline MicroResult
run_micro_benchmark(std::function<void()> setup, std::function<void()> op,
                    uint64_t op_count) {
    MicroResult result;
    result.iterations = op_count;
    if (op_count == 0)
        return result;

    setup();
    auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < op_count; ++i) {
        op();
    }
    auto end = std::chrono::steady_clock::now();

    auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    result.runtime_ns = static_cast<uint64_t>(ns);
    if (ns > 0) {
        result.ops_per_sec = static_cast<uint64_t>(
            static_cast<double>(op_count) * 1e9 / static_cast<double>(ns));
    }
    return result;
}

inline TrialMetrics
run_message_creation_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    constexpr uint64_t kOps = 100000;
    BenchPayloadHeader header{1, 2, 3};
    size_t payload_size = std::max(static_cast<size_t>(cfg.message_size_bytes),
                                   BenchPayloadHeader::kEncodedSize);
    uint64_t counter = 0;

    auto setup = [&] { counter = 0; };
    auto op = [&] {
        auto payload = encode_shaped_payload(header, cfg.message_size_bytes,
                                             cfg.message_shape, cfg.seed);
        auto msg = make_bench_msg(MailboxLoadTag, std::move(payload));
        ++counter;
    };

    auto result = run_micro_benchmark(setup, op, kOps);
    metrics.throughput_msgps = static_cast<double>(result.ops_per_sec);
    metrics.total_sent = result.iterations;
    metrics.total_received = result.iterations;
    metrics.bytes_sent = result.iterations * payload_size;
    metrics.runtime_ms = result.runtime_ns / 1000000;
    metrics.completed = (counter == kOps);
    return metrics;
}

} // namespace hpactor::apps::bench_caf
