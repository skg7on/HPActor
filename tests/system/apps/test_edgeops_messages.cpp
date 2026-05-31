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

#include <apps/edgeops_telemetry/alert_rules.hpp>
#include <apps/edgeops_telemetry/messages.hpp>
#include <apps/edgeops_telemetry/rollup.hpp>
#include <apps/edgeops_telemetry/scenario.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace hpactor::apps::edgeops_telemetry;

TEST(EdgeOpsMessagesTest, TypeTagsUseDedicatedExampleRange) {
    EXPECT_EQ(static_cast<uint32_t>(DeviceRegisterTag), 0x00030000u);
    EXPECT_EQ(static_cast<uint32_t>(TelemetryReadingTag), 0x00030003u);
    EXPECT_EQ(static_cast<uint32_t>(WindowRollupTag), 0x00030006u);
    EXPECT_EQ(static_cast<uint32_t>(FleetSummaryTag), 0x0003000Du);
}

TEST(EdgeOpsMessagesTest, TelemetryReadingRoundTrip) {
    TelemetryReadingPayload in;
    in.device_id = "device-001";
    in.site_id = "edge-a";
    in.sensor_type = SensorType::Temperature;
    in.sequence = 42;
    in.timestamp_ns = 123456789;
    in.reading_milli = 78500;
    in.quality_flags = 7;
    in.scenario = ScenarioKind::Overload;

    auto encoded = encode_telemetry_reading(in);
    TelemetryReadingPayload out;
    ASSERT_TRUE(decode_telemetry_reading(encoded, out));
    EXPECT_EQ(out.device_id, "device-001");
    EXPECT_EQ(out.site_id, "edge-a");
    EXPECT_EQ(out.sensor_type, SensorType::Temperature);
    EXPECT_EQ(out.sequence, 42u);
    EXPECT_EQ(out.timestamp_ns, 123456789u);
    EXPECT_EQ(out.reading_milli, 78500);
    EXPECT_EQ(out.quality_flags, 7u);
    EXPECT_EQ(out.scenario, ScenarioKind::Overload);
}

TEST(EdgeOpsMessagesTest, MalformedDecodeRejected) {
    hpactor::StreamBuffer truncated{0x00, 0x00, 0x00, 0x08, 'd'};
    TelemetryReadingPayload reading;
    EXPECT_FALSE(decode_telemetry_reading(truncated, reading));

    DeviceRegisterPayload device;
    EXPECT_FALSE(decode_device_register(truncated, device));
}

TEST(EdgeOpsMessagesTest, ScenarioParsingCoversOperationalDrills) {
    EXPECT_EQ(scenario_from_string("happy-path"), ScenarioKind::HappyPath);
    EXPECT_EQ(scenario_from_string("device-churn"), ScenarioKind::DeviceChurn);
    EXPECT_EQ(scenario_from_string("malformed-telemetry"),
              ScenarioKind::MalformedTelemetry);
    EXPECT_EQ(scenario_from_string("overload"), ScenarioKind::Overload);
    EXPECT_EQ(scenario_from_string("missing-route"), ScenarioKind::MissingRoute);
    EXPECT_EQ(scenario_from_string("timer-rollup"), ScenarioKind::TimerRollup);
    EXPECT_EQ(scenario_from_string("processor-restart"),
              ScenarioKind::ProcessorRestart);
    EXPECT_EQ(scenario_from_string("graceful-shutdown"),
              ScenarioKind::GracefulShutdown);
    EXPECT_EQ(scenario_from_string("fault-injection"), ScenarioKind::FaultInjection);
    EXPECT_EQ(scenario_from_string("unknown"), ScenarioKind::HappyPath);
    EXPECT_EQ(std::string(to_string(ScenarioKind::TimerRollup)), "timer-"
                                                                 "rollup");
}

TEST(EdgeOpsMessagesTest, RollupAccumulatorComputesWindowStats) {
    RollupAccumulator rollup("edge-a", SensorType::Temperature);
    rollup.add(NormalizedReadingPayload{"device-001", "edge-a",
                                        SensorType::Temperature, 1, 100, 1000,
                                        0, ScenarioKind::HappyPath});
    rollup.add(NormalizedReadingPayload{"device-002", "edge-a",
                                        SensorType::Temperature, 2, 200, 1600,
                                        0, ScenarioKind::HappyPath});
    rollup.add(NormalizedReadingPayload{"device-003", "edge-a",
                                        SensorType::Temperature, 3, 300, 1300,
                                        0, ScenarioKind::HappyPath});

    auto window = rollup.finish(100, 400);
    EXPECT_EQ(window.site_id, "edge-a");
    EXPECT_EQ(window.sensor_type, SensorType::Temperature);
    EXPECT_EQ(window.count, 3u);
    EXPECT_EQ(window.min_milli, 1000);
    EXPECT_EQ(window.max_milli, 1600);
    EXPECT_EQ(window.sum_milli, 3900);
    EXPECT_EQ(window.average_milli, 1300);
}

TEST(EdgeOpsMessagesTest, AlertRulesEmitThresholdAndRateAlerts) {
    NormalizedReadingPayload previous{
        "device-001", "edge-a", SensorType::Temperature, 10, 1000,
        70000,        0,        ScenarioKind::HappyPath};
    NormalizedReadingPayload current{
        "device-001", "edge-a", SensorType::Temperature, 11, 2000,
        82000,        0,        ScenarioKind::HappyPath};

    ThresholdRule threshold{SensorType::Temperature, 80000, "temperature-high"};
    AlertRaisedPayload alert;
    ASSERT_TRUE(threshold.evaluate(current, alert));
    EXPECT_EQ(alert.device_id, "device-001");
    EXPECT_EQ(alert.reason, "temperature-high");
    EXPECT_EQ(alert.threshold_milli, 80000);
    EXPECT_EQ(alert.reading_milli, 82000);

    RateOfChangeRule rate{SensorType::Temperature, 10000, "temperature-jump"};
    AlertRaisedPayload rate_alert;
    ASSERT_TRUE(rate.evaluate(previous, current, rate_alert));
    EXPECT_EQ(rate_alert.reason, "temperature-jump");
    EXPECT_EQ(rate_alert.threshold_milli, 10000);
    EXPECT_EQ(rate_alert.reading_milli, 12000);
}

TEST(EdgeOpsMessagesTest, DefaultScenarioConfigIsDeterministic) {
    auto config = default_scenario_config(ScenarioKind::Overload);
    EXPECT_EQ(config.scenario, ScenarioKind::Overload);
    EXPECT_EQ(config.device_count, 8u);
    EXPECT_EQ(config.readings_per_device, 4u);
    EXPECT_EQ(config.storage_capacity, 8u);
}
