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

TEST(Mandelbrot, SmokeCompletesComputation) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::Mandelbrot;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;

    auto metrics = bench_caf::run_mandelbrot_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_GT(metrics.actors_created, 0u);
    EXPECT_GT(metrics.runtime_ms, 0u);
}
