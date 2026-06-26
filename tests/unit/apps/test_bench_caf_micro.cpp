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

#include <apps/bench_caf/caf_bench_config.hpp>
#include <apps/bench_caf/caf_bench_micro.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(MicroBenchmark, ComputesOpsPerSec) {
    uint64_t counter = 0;
    auto result =
        bench_caf::run_micro_benchmark([] {}, [&counter] { ++counter; }, 1000000);
    EXPECT_GT(result.ops_per_sec, 0u);
    EXPECT_EQ(counter, 1000000u);
    EXPECT_GT(result.runtime_ns, 0u);
}

TEST(MicroBenchmark, EmptyOperationReturnsZero) {
    auto result = bench_caf::run_micro_benchmark([] {}, [] {}, 0);
    EXPECT_EQ(result.ops_per_sec, 0u);
    EXPECT_EQ(result.iterations, 0u);
}

TEST(MessageCreation, SmokeCompletes) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MessageCreation;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.message_size_bytes = 0;

    auto metrics = bench_caf::run_message_creation_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_GT(metrics.throughput_msgps, 0.0);
}
