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

#include <cstdio>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <hpactor/tracing/json_exporter.hpp>
#include <hpactor/tracing/otlp_exporter.hpp>

using namespace hpactor;
using namespace hpactor::tracing;

static SpanRecord make_record() {
    SpanRecord r;
    r.trace_id.bytes[15] = 1;
    r.span_id.bytes[7] = 2;
    r.actor_id = ActorId{42};
    r.type_tag = static_cast<uint32_t>(TypeTag::User);
    r.start_ns = 10;
    r.end_ns = 20;
    r.kind = SpanKind::kConsumer;
    r.status = SpanStatus::kOk;
    return r;
}

TEST(TraceExportersTest, JsonFileExporterWritesValidJson) {
    const char* path = "/tmp/hpactor-trace-exporter-test.jsonl";
    std::remove(path);
    JsonFileExporter json(path);
    SpanRecord record = make_record();
    auto res = json.export_batch(std::span<const SpanRecord>(&record, 1));
    EXPECT_TRUE(res.has_value());
    json.shutdown();

    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    EXPECT_NE(line.find("\"trace_id\""), std::string::npos);
    EXPECT_NE(line.find("\"actor_id\":42"), std::string::npos);
}

TEST(TraceExportersTest, OtlpHttpExporterBuildsValidPayload) {
    OtlpHttpExporter otlp("http://127.0.0.1:4318/v1/traces");
    SpanRecord record = make_record();
    std::string body = otlp.build_json_payload_for_test(
        std::span<const SpanRecord>(&record, 1), "hpactor-test");
    EXPECT_NE(body.find("\"resourceSpans\""), std::string::npos);
    EXPECT_NE(body.find("\"traceId\""), std::string::npos);
    EXPECT_NE(body.find("\"spanId\""), std::string::npos);
}