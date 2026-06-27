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
#include <apps/bench_caf/caf_bench_scenarios.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafMixedCase, SmokeCompletesRingsAndCpuWork) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MixedCase;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.sample_rss_ms = 10;

    auto metrics = bench_caf::run_mixed_case_trial(cfg, 1);

    EXPECT_TRUE(metrics.completed);
    EXPECT_EQ(metrics.trial, 1u);
    EXPECT_EQ(metrics.rings_completed, 4u);
    EXPECT_EQ(metrics.cpu_tasks_completed, 4u);
    EXPECT_EQ(metrics.token_hops, 100u);
    EXPECT_GT(metrics.actors_created, 0u);
}
