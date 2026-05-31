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

#include <apps/edgeops_telemetry/scenario.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace hpactor::apps::edgeops_telemetry;

TEST(EdgeOpsTelemetrySystemTest, HappyPathStoresTelemetryAndEmitsRollup) {
    auto config = default_scenario_config(ScenarioKind::HappyPath);
    config.device_count = 4;
    config.readings_per_device = 3;
    auto summary = run_scenario(config);

    EXPECT_EQ(summary.status, "completed");
    EXPECT_EQ(summary.devices_registered, 4u);
    EXPECT_EQ(summary.readings_received, 12u);
    EXPECT_EQ(summary.readings_normalized, 12u);
    EXPECT_EQ(summary.readings_stored, 12u);
    EXPECT_EQ(summary.readings_rejected, 0u);
    EXPECT_GE(summary.rollups_emitted, 1u);
    EXPECT_GE(summary.actor_count, 6u);
    EXPECT_GT(summary.scheduler_workers, 0u);
}

TEST(EdgeOpsTelemetrySystemTest, MalformedTelemetryIsRejected) {
    auto summary =
        run_scenario(default_scenario_config(ScenarioKind::MalformedTelemetry));

    EXPECT_EQ(summary.status, "completed-with-rejections");
    EXPECT_GT(summary.readings_received, summary.readings_normalized);
    EXPECT_GT(summary.readings_rejected, 0u);
    EXPECT_EQ(summary.readings_dropped, 0u);
}

TEST(EdgeOpsTelemetrySystemTest, OverloadReportsStoragePressureAndDlq) {
    auto summary = run_scenario(default_scenario_config(ScenarioKind::Overload));

    EXPECT_EQ(summary.status, "completed-with-pressure");
    EXPECT_GT(summary.readings_dropped, 0u);
    EXPECT_EQ(summary.storage_peak_depth, summary.storage_capacity);
    EXPECT_GT(summary.dlq_total_pushed, 0u);
}

TEST(EdgeOpsTelemetrySystemTest, MissingRouteUsesRuntimeFailureEvidence) {
    auto summary =
        run_scenario(default_scenario_config(ScenarioKind::MissingRoute));

    EXPECT_EQ(summary.status, "missing-route");
    EXPECT_EQ(summary.readings_stored, 0u);
    EXPECT_GT(summary.dlq_depth, 0u);
    EXPECT_GT(summary.dlq_total_pushed, 0u);
}

TEST(EdgeOpsTelemetrySystemTest, TimerRollupEmitsWindowWithoutDataLoss) {
    auto summary =
        run_scenario(default_scenario_config(ScenarioKind::TimerRollup));

    EXPECT_EQ(summary.status, "completed");
    EXPECT_EQ(summary.readings_received, summary.readings_stored);
    EXPECT_GE(summary.rollups_emitted, 2u);
}

TEST(EdgeOpsTelemetrySystemTest, QuerySummaryProjectsFleetCounters) {
    auto summary = run_scenario(default_scenario_config(ScenarioKind::HappyPath));
    auto fleet = to_fleet_summary(summary);

    EXPECT_EQ(fleet.devices_registered, summary.devices_registered);
    EXPECT_EQ(fleet.readings_received, summary.readings_received);
    EXPECT_EQ(fleet.readings_stored, summary.readings_stored);
    EXPECT_EQ(fleet.alerts_raised, summary.alerts_raised);
}

TEST(EdgeOpsTelemetrySystemTest, GracefulShutdownDrainsFiniteWork) {
    auto summary =
        run_scenario(default_scenario_config(ScenarioKind::GracefulShutdown));

    EXPECT_EQ(summary.status, "drained");
    EXPECT_TRUE(summary.drained);
    EXPECT_EQ(summary.readings_received, summary.readings_stored +
                                             summary.readings_rejected +
                                             summary.readings_dropped);
}
