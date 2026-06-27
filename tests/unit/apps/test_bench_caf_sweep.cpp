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
#include <apps/bench_caf/caf_bench_sweep.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(SweepExpansion, NightlyMailboxN1ExpandsToSixSizes) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MailboxN1;
    cfg.preset = bench_caf::PresetKind::Nightly;

    auto sweep = bench_caf::expand_sweep(cfg);
    EXPECT_EQ(sweep.size(), 6u);

    EXPECT_EQ(sweep[0].config.message_size_bytes, 0u);
    EXPECT_EQ(sweep[0].config.message_shape, bench_caf::MessageShape::HeaderOnly);

    EXPECT_EQ(sweep[1].config.message_size_bytes, 16u);
    EXPECT_EQ(sweep[1].config.message_shape, bench_caf::MessageShape::FixedBytes);

    EXPECT_EQ(sweep[5].config.message_size_bytes, 4096u);
}

TEST(SweepExpansion, SmokePassthrough) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::ActorCreation;
    cfg.preset = bench_caf::PresetKind::Smoke;

    auto sweep = bench_caf::expand_sweep(cfg);
    ASSERT_EQ(sweep.size(), 1u);
    EXPECT_EQ(sweep[0].config.scenario, bench_caf::ScenarioKind::ActorCreation);
}

TEST(SweepExpansion, PaperScaleMailboxN1ExpandsToMoreSizes) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MailboxN1;
    cfg.preset = bench_caf::PresetKind::PaperScale;

    auto sweep = bench_caf::expand_sweep(cfg);
    EXPECT_GE(sweep.size(), 4u);
}

TEST(SweepExpansion, StressPassthrough) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MailboxN1;
    cfg.preset = bench_caf::PresetKind::Stress;

    auto sweep = bench_caf::expand_sweep(cfg);
    ASSERT_EQ(sweep.size(), 1u);
}
