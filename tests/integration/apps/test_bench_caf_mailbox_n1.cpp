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

TEST(BenchCafMailboxN1, SmokeCompletesAllMessages) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MailboxN1;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.mailbox_capacity = 4096;
    cfg.sample_rss_ms = 10;

    auto metrics = bench_caf::run_mailbox_n1_trial(cfg, 1);

    EXPECT_TRUE(metrics.completed);
    EXPECT_EQ(metrics.trial, 1u);
    EXPECT_EQ(metrics.total_sent, 40000u);
    EXPECT_EQ(metrics.total_received, 40000u);
    EXPECT_EQ(metrics.total_dropped, 0u);
    EXPECT_GT(metrics.throughput_msgps, 0.0);
}
