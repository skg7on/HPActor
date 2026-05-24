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

} // anonymous namespace

// ── Sampler kind parsing ─────────────────────────────────────────

TEST(TracingConfigParserTest, SamplerAlwaysOff) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]
enabled = true
sampler = "always_off"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_sampler_off");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.tracing.sampler,
              hpactor::tracing::SamplerKind::kAlwaysOff);
}

TEST(TracingConfigParserTest, SamplerAlwaysOn) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]
enabled = true
sampler = "always_on"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_sampler_on");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.tracing.sampler,
              hpactor::tracing::SamplerKind::kAlwaysOn);
}

TEST(TracingConfigParserTest, SamplerTraceIdRatio) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]
enabled = true
sampler = "trace_id_ratio"
sample_ratio = 0.5

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_sampler_ratio");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.tracing.sampler,
              hpactor::tracing::SamplerKind::kTraceIdRatio);
    EXPECT_DOUBLE_EQ(result.value().system.tracing.sample_ratio, 0.5);
}

TEST(TracingConfigParserTest, SamplerDefault) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]
enabled = true
sampler = "unknown_kind"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_sampler_default");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.tracing.sampler,
              hpactor::tracing::SamplerKind::kParentBasedTraceIdRatio);
}

// ── Exporter kind parsing ────────────────────────────────────────

TEST(TracingConfigParserTest, ExporterNoop) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]
enabled = true
exporter = "noop"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_exporter_noop");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.tracing.exporter,
              hpactor::tracing::TraceExporterKind::kNoop);
}

TEST(TracingConfigParserTest, ExporterMemory) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]
enabled = true
exporter = "memory"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_exporter_memory");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.tracing.exporter,
              hpactor::tracing::TraceExporterKind::kMemory);
}

TEST(TracingConfigParserTest, ExporterJsonFile) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]
enabled = true
exporter = "json_file"
json_file_path = "/tmp/traces.json"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_exporter_json");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.tracing.exporter,
              hpactor::tracing::TraceExporterKind::kJsonFile);
    EXPECT_EQ(result.value().system.tracing.json_file_path, "/tmp/traces.json");
}

TEST(TracingConfigParserTest, ExporterDefault) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]
enabled = true
exporter = "bogus"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_exporter_default");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().system.tracing.exporter,
              hpactor::tracing::TraceExporterKind::kOtlpHttp);
}

// ── Full config parsing ──────────────────────────────────────────

TEST(TracingConfigParserTest, FullConfig) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]
enabled = true
service_name = "test-service"
propagate_unsampled = false
ring_buffer_capacity = 32768
sampler = "always_on"
exporter = "memory"
otlp_endpoint = "http://localhost:4318/v1/traces"
json_file_path = "/var/log/traces.json"
export_interval_ms = 1000
max_export_batch_size = 256
max_tracestate_len = 128
record_actor_receive_spans = false
record_remote_producer_spans = false
record_local_producer_spans = true
record_payload_size = false
create_roots_for_actor_context_sends = true
create_roots_for_rpc = false
create_roots_for_http_ingress = false
sample_ratio = 0.25

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_full");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& cfg = result.value().system.tracing;
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.service_name, "test-service");
    EXPECT_FALSE(cfg.propagate_unsampled);
    EXPECT_EQ(cfg.ring_buffer_capacity, 32768u);
    EXPECT_EQ(cfg.sampler, hpactor::tracing::SamplerKind::kAlwaysOn);
    EXPECT_EQ(cfg.exporter, hpactor::tracing::TraceExporterKind::kMemory);
    EXPECT_EQ(cfg.otlp_endpoint, "http://localhost:4318/v1/traces");
    EXPECT_EQ(cfg.json_file_path, "/var/log/traces.json");
    EXPECT_EQ(cfg.export_interval, std::chrono::milliseconds(1000));
    EXPECT_EQ(cfg.max_export_batch_size, 256u);
    EXPECT_EQ(cfg.max_tracestate_len, 128u);
    EXPECT_FALSE(cfg.record_actor_receive_spans);
    EXPECT_FALSE(cfg.record_remote_producer_spans);
    EXPECT_TRUE(cfg.record_local_producer_spans);
    EXPECT_FALSE(cfg.record_payload_size);
    EXPECT_TRUE(cfg.create_roots_for_actor_context_sends);
    EXPECT_FALSE(cfg.create_roots_for_rpc);
    EXPECT_FALSE(cfg.create_roots_for_http_ingress);
    EXPECT_DOUBLE_EQ(cfg.sample_ratio, 0.25);
}

// ── Default values ───────────────────────────────────────────────

TEST(TracingConfigParserTest, AbsentSectionUsesDefaults) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_absent");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& cfg = result.value().system.tracing;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.service_name, "hpactor");
    EXPECT_TRUE(cfg.propagate_unsampled);
    EXPECT_EQ(cfg.ring_buffer_capacity, 65536u);
}

TEST(TracingConfigParserTest, EmptyTracingSectionUsesDefaults) {
    std::string content = R"(
[system]
version = "1.0"

[system.tracing]

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "tracing_empty");
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& cfg = result.value().system.tracing;
    // The section exists but has no fields; all defaults should apply
    EXPECT_EQ(cfg.service_name, "hpactor");
    EXPECT_EQ(cfg.ring_buffer_capacity, 65536u);
}
