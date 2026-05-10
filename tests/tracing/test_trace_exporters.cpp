#include <hpactor/tracing/json_exporter.hpp>
#include <hpactor/tracing/otlp_exporter.hpp>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

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

int main() {
    const char* path = "/tmp/hpactor-trace-exporter-test.jsonl";
    std::remove(path);
    JsonFileExporter json(path);
    SpanRecord record = make_record();
    auto res = json.export_batch(std::span<const SpanRecord>(&record, 1));
    assert(res.has_value());
    json.shutdown();

    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    assert(line.find("\"trace_id\"") != std::string::npos);
    assert(line.find("\"actor_id\":42") != std::string::npos);

    OtlpHttpExporter otlp("http://127.0.0.1:4318/v1/traces");
    std::string body = otlp.build_json_payload_for_test(
        std::span<const SpanRecord>(&record, 1), "hpactor-test");
    assert(body.find("\"resourceSpans\"") != std::string::npos);
    assert(body.find("\"traceId\"") != std::string::npos);
    assert(body.find("\"spanId\"") != std::string::npos);
    return 0;
}
