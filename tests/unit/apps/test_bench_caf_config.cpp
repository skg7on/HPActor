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
#include <apps/bench_caf/caf_bench_metrics.hpp>
#include <apps/bench_caf/caf_bench_output.hpp>

#include <gtest/gtest.h>

#include <string>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafConfig, ParsesMailboxSmokeJson) {
    const char* argv[] = {
        "18_bench_caf",
        "--scenario",
        "mailbox-n1",
        "--preset",
        "smoke",
        "--scheduler-threads",
        "4",
        "--message-size",
        "1024",
        "--message-shape",
        "fixed-bytes",
        "--trials",
        "3",
        "--format",
        "json",
    };

    auto parsed = bench_caf::parse_caf_bench_args(15, argv);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.config.scenario, bench_caf::ScenarioKind::MailboxN1);
    EXPECT_EQ(parsed.config.preset, bench_caf::PresetKind::Smoke);
    EXPECT_EQ(parsed.config.scheduler_threads, 4u);
    EXPECT_EQ(parsed.config.message_size_bytes, 1024u);
    EXPECT_EQ(parsed.config.message_shape, bench_caf::MessageShape::FixedBytes);
    EXPECT_EQ(parsed.config.trials, 3u);
    EXPECT_EQ(parsed.config.format, bench_caf::OutputFormat::Json);
}

TEST(BenchCafConfig, RejectsZeroTrials) {
    const char* argv[] = {
        "18_bench_caf", "--scenario", "actor-creation", "--trials", "0",
    };

    auto parsed = bench_caf::parse_caf_bench_args(5, argv);
    ASSERT_FALSE(parsed.ok);
    EXPECT_NE(parsed.error.find("trials must be greater than zero"),
              std::string::npos);
}

TEST(BenchCafOutput, JsonContainsScenarioAndTrialCounters) {
    bench_caf::CafBenchReport report;
    report.config.scenario = bench_caf::ScenarioKind::ActorCreation;
    report.config.preset = bench_caf::PresetKind::Smoke;
    report.config.scheduler_threads = 2;

    bench_caf::TrialMetrics trial;
    trial.trial = 1;
    trial.completed = true;
    trial.runtime_ms = 7;
    trial.actors_created = 2047;
    trial.actors_completed = 2047;
    report.trials.push_back(trial);

    auto json = bench_caf::write_json_report(report);
    EXPECT_NE(json.find("\"schema_version\": \"1\""), std::string::npos);
    EXPECT_NE(json.find("\"scenario\": \"actor-creation\""), std::string::npos);
    EXPECT_NE(json.find("\"actors_created\": 2047"), std::string::npos);
}

TEST(BenchCafOutput, CsvHasStableHeaderAndOneTrialRow) {
    bench_caf::CafBenchReport report;
    report.config.scenario = bench_caf::ScenarioKind::MailboxN1;
    report.config.preset = bench_caf::PresetKind::Smoke;
    report.config.scheduler_threads = 2;

    bench_caf::TrialMetrics trial;
    trial.trial = 1;
    trial.completed = true;
    trial.total_sent = 10;
    trial.total_received = 10;
    report.trials.push_back(trial);

    auto csv = bench_caf::write_csv_report(report);
    EXPECT_NE(csv.find("scenario,preset,trial,completed"), std::string::npos);
    EXPECT_NE(csv.find("mailbox-n1,smoke,1,true"), std::string::npos);
}
