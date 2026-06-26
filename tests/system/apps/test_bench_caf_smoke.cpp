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
#include <apps/bench_caf/caf_bench_runner.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafRunner, RunsActorCreationTrial) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::ActorCreation;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.trials = 1;

    auto report = bench_caf::run_caf_benchmark(cfg);

    ASSERT_EQ(report.trials.size(), 1u);
    EXPECT_TRUE(report.trials[0].completed);
    EXPECT_EQ(report.trials[0].actors_created, 2047u);
}

TEST(BenchCafRunner, RunsMailboxTrial) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MailboxN1;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.trials = 1;

    auto report = bench_caf::run_caf_benchmark(cfg);

    ASSERT_EQ(report.trials.size(), 1u);
    EXPECT_TRUE(report.trials[0].completed);
    EXPECT_EQ(report.trials[0].total_sent, 40000u);
    EXPECT_EQ(report.trials[0].total_received, 40000u);
}

TEST(BenchCafRunner, RunsMixedCaseTrial) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MixedCase;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.trials = 1;

    auto report = bench_caf::run_caf_benchmark(cfg);

    ASSERT_EQ(report.trials.size(), 1u);
    EXPECT_TRUE(report.trials[0].completed);
    EXPECT_EQ(report.trials[0].rings_completed, 4u);
}
