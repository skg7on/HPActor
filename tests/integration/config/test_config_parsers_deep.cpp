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

#include <hpactor/config/toml_parser.hpp>
#include <hpactor/process/process_config.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <string>

using namespace hpactor;
using namespace hpactor::config;

namespace {

std::string write_temp(const std::string& content, const std::string& name) {
    std::string path = "/tmp/hpactor_test_" + name + ".toml";
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

// Helper to produce a minimal valid document with an extra system subsection.
std::string minimal_with(const std::string& extra_section) {
    return R"(
[system]
version = "1.0"

)" + extra_section +
           R"(

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
}

} // anonymous namespace

// ── Delivery config parser ─────────────────────────────────────────

TEST(ConfigParsersDeepTest, DeliveryConfigDefaults) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "delivery_defaults");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    const auto& d = result.value().system.delivery;
    EXPECT_EQ(d.default_mode, mailbox::DeliveryMode::BestEffort);
    EXPECT_EQ(d.max_retries, 3u);
    EXPECT_EQ(d.retry_backoff_ms, 100u);
    EXPECT_EQ(d.retry_backoff_max_ms, 10000u);
    EXPECT_EQ(d.dedup_window_ms, 300000u);
    EXPECT_EQ(d.dedup_max_entries, 65536u);
    EXPECT_EQ(d.default_message_ttl_ms, 0u);
}

TEST(ConfigParsersDeepTest, DeliveryConfigCustomValues) {
    std::string content = minimal_with(R"(
[system.delivery]
default_mode = "at_least_once"
max_retries = 10
retry_backoff_ms = 200
retry_backoff_max_ms = 60000
dedup_window_ms = 60000
dedup_max_entries = 32768
default_message_ttl_ms = 30000
)");
    std::string path = write_temp(content, "delivery_custom");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    const auto& d = result.value().system.delivery;
    EXPECT_EQ(d.default_mode, mailbox::DeliveryMode::AtLeastOnce);
    EXPECT_EQ(d.max_retries, 10u);
    EXPECT_EQ(d.retry_backoff_ms, 200u);
    EXPECT_EQ(d.retry_backoff_max_ms, 60000u);
    EXPECT_EQ(d.dedup_window_ms, 60000u);
    EXPECT_EQ(d.dedup_max_entries, 32768u);
    EXPECT_EQ(d.default_message_ttl_ms, 30000u);
}

TEST(ConfigParsersDeepTest, DeliveryConfigObservableBestEffort) {
    std::string content = minimal_with(R"(
[system.delivery]
default_mode = "observable_best_effort"
)");
    std::string path = write_temp(content, "delivery_observable");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.delivery.default_mode,
              mailbox::DeliveryMode::ObservableBestEffort);
}

TEST(ConfigParsersDeepTest, DeliveryConfigDurableAtLeastOnce) {
    std::string content = minimal_with(R"(
[system.delivery]
default_mode = "durable_at_least_once"
)");
    std::string path = write_temp(content, "delivery_durable");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.delivery.default_mode,
              mailbox::DeliveryMode::DurableAtLeastOnce);
}

TEST(ConfigParsersDeepTest, DeliveryConfigUnknownModeDefaultsBestEffort) {
    std::string content = minimal_with(R"(
[system.delivery]
default_mode = "nonexistent_mode"
)");
    std::string path = write_temp(content, "delivery_unknown");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.delivery.default_mode,
              mailbox::DeliveryMode::BestEffort);
}

// ── Quarantine config parser ───────────────────────────────────────

TEST(ConfigParsersDeepTest, QuarantineConfigDefaults) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "quarantine_defaults");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    const auto& q = result.value().system.quarantine_defaults;
    EXPECT_FALSE(q.enabled);
    EXPECT_TRUE(q.escalate_on_max_restarts);
    EXPECT_EQ(q.failure_rate_threshold, 0u);
    EXPECT_EQ(q.timeout_rate_threshold, 0u);
    EXPECT_FLOAT_EQ(q.mailbox_pressure_threshold, 0.0f);
    EXPECT_EQ(q.cooldown_period, std::chrono::milliseconds(30000));
    EXPECT_EQ(q.observation_window, std::chrono::milliseconds(10000));
    EXPECT_EQ(q.max_circuit_trips, 3u);
}

TEST(ConfigParsersDeepTest, QuarantineConfigCustomValues) {
    std::string content = minimal_with(R"(
[system.quarantine]
default_enabled = true
default_escalate_on_max_restarts = false
default_failure_rate_threshold = 5
default_timeout_rate_threshold = 10
default_mailbox_pressure_threshold = 0.75
default_cooldown_period_ms = 60000
default_observation_window_ms = 30000
default_max_circuit_trips = 8
)");
    std::string path = write_temp(content, "quarantine_custom");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    const auto& q = result.value().system.quarantine_defaults;
    EXPECT_TRUE(q.enabled);
    EXPECT_FALSE(q.escalate_on_max_restarts);
    EXPECT_EQ(q.failure_rate_threshold, 5u);
    EXPECT_EQ(q.timeout_rate_threshold, 10u);
    EXPECT_FLOAT_EQ(q.mailbox_pressure_threshold, 0.75f);
    EXPECT_EQ(q.cooldown_period, std::chrono::milliseconds(60000));
    EXPECT_EQ(q.observation_window, std::chrono::milliseconds(30000));
    EXPECT_EQ(q.max_circuit_trips, 8u);
}

// ── Process config parser ──────────────────────────────────────────

TEST(ConfigParsersDeepTest, ProcessConfigDefaults) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "process_defaults");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    const auto& pc = result.value().system.process;
    EXPECT_EQ(pc.mode, hpactor::process::ProcessMode::Foreground);
    EXPECT_TRUE(pc.pidfile_path.empty());
    EXPECT_FALSE(pc.redirect_stdio);
    EXPECT_TRUE(pc.log_file.empty());
    EXPECT_EQ(pc.working_directory, "/");
    EXPECT_EQ(pc.watchdog_interval, std::chrono::milliseconds(0));
}

TEST(ConfigParsersDeepTest, ProcessConfigCustomValues) {
    std::string content = minimal_with(R"(
[system.process]
mode = "daemon"
pidfile = "/var/run/hpactor.pid"
redirect_stdio = true
log_file = "/var/log/hpactor.log"
working_directory = "/var/lib/hpactor"
watchdog_interval_ms = 30000
)");
    std::string path = write_temp(content, "process_custom");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    const auto& pc = result.value().system.process;
    EXPECT_EQ(pc.mode, hpactor::process::ProcessMode::Daemon);
    EXPECT_EQ(pc.pidfile_path, "/var/run/hpactor.pid");
    EXPECT_TRUE(pc.redirect_stdio);
    EXPECT_EQ(pc.log_file, "/var/log/hpactor.log");
    EXPECT_EQ(pc.working_directory, "/var/lib/hpactor");
    EXPECT_EQ(pc.watchdog_interval, std::chrono::milliseconds(30000));
}

// ── Mailbox watermark clamping ─────────────────────────────────────

TEST(ConfigParsersDeepTest, MailboxWatermarkClampingLowNegative) {
    // low_watermark < 0 should clamp to 0.50.
    std::string content = minimal_with(R"(
[system.mailbox]
low_watermark = -0.5
high_watermark = 0.80
critical_watermark = 1.00
)");
    std::string path = write_temp(content, "mailbox_watermark_low");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result.value().system.mailbox.low_watermark, 0.50);
}

TEST(ConfigParsersDeepTest, MailboxWatermarkClampingHighBelowLow) {
    // high_watermark < low_watermark should clamp high to 0.80.
    std::string content = minimal_with(R"(
[system.mailbox]
low_watermark = 0.90
high_watermark = 0.50
)");
    std::string path = write_temp(content, "mailbox_watermark_high");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    // low 0.90 is valid (> 0), stays
    EXPECT_DOUBLE_EQ(result.value().system.mailbox.low_watermark, 0.90);
    // high < low, clamped to 0.80
    EXPECT_DOUBLE_EQ(result.value().system.mailbox.high_watermark, 0.80);
}

TEST(ConfigParsersDeepTest, MailboxWatermarkClampingCriticalInvalid) {
    // critical < high or > 1.0 should clamp to 1.00.
    std::string content = minimal_with(R"(
[system.mailbox]
high_watermark = 0.70
critical_watermark = 0.60
)");
    std::string path = write_temp(content, "mailbox_watermark_critical");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    // critical < high → clamped to 1.00
    EXPECT_DOUBLE_EQ(result.value().system.mailbox.critical_watermark, 1.00);
}

// ── AI accelerator config error paths ──────────────────────────────

TEST(ConfigParsersDeepTest, AiAcceleratorInvalidAdmissionPolicy) {
    std::string content = minimal_with(R"(
[system.ai.accelerators]
enabled = true
admission_policy = "invalid_policy"
)");
    std::string path = write_temp(content, "ai_invalid_policy");
    auto result = TomlParser::parse(path);
    EXPECT_FALSE(result.has_value());
}

TEST(ConfigParsersDeepTest, AiAcceleratorTtlBoundsInvalid) {
    // min > max should fail.
    std::string content = minimal_with(R"(
[system.ai.accelerators]
enabled = true
min_lease_ttl_ms = 10000
max_lease_ttl_ms = 1000
)");
    std::string path = write_temp(content, "ai_ttl_bounds");
    auto result = TomlParser::parse(path);
    EXPECT_FALSE(result.has_value());
}

TEST(ConfigParsersDeepTest, AiAcceleratorDuplicateMockDeviceIds) {
    std::string content = minimal_with(R"(
[system.ai.accelerators]
enabled = true

[[system.ai.accelerators.mock_device]]
id = "gpu0"
kind = "gpu"
memory_mb = 4096

[[system.ai.accelerators.mock_device]]
id = "gpu0"
kind = "gpu"
memory_mb = 8192
)");
    std::string path = write_temp(content, "ai_duplicate_ids");
    auto result = TomlParser::parse(path);
    EXPECT_FALSE(result.has_value());
}

// ── Discovery config parser ────────────────────────────────────────

TEST(ConfigParsersDeepTest, DiscoveryConfigBackend) {
    std::string content = minimal_with(R"(
[system.discovery]
backend = "gossip"
)");
    std::string path = write_temp(content, "discovery_gossip");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.discovery_backend, "gossip");
}

TEST(ConfigParsersDeepTest, DiscoveryConfigDefaultsToEmpty) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "discovery_default");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().system.discovery_backend.empty());
}

// ── Tracing config edge cases ──────────────────────────────────────

TEST(ConfigParsersDeepTest, TracingConfigUnknownExporterDefaults) {
    std::string content = minimal_with(R"(
[system.tracing]
enabled = true
exporter = "unknown_exporter"
)");
    std::string path = write_temp(content, "tracing_unknown_exporter");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.tracing.exporter,
              tracing::TraceExporterKind::kOtlpHttp);
}

// ── CLI config parser edge cases ───────────────────────────────────

TEST(ConfigParsersDeepTest, CliConfigDisabled) {
    std::string content = minimal_with(R"(
[system.cli]
enabled = false
)");
    std::string path = write_temp(content, "cli_disabled");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value().system.cli.enabled);
}

TEST(ConfigParsersDeepTest, CliConfigCustomFormat) {
    std::string content = minimal_with(R"(
[system.cli]
default_format = "json"
page_size = 100
)");
    std::string path = write_temp(content, "cli_format");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.cli.default_format, "json");
    EXPECT_EQ(result.value().system.cli.page_size, 100u);
}
