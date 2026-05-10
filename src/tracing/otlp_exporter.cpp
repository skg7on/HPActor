#include <hpactor/net/http_client.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/tracing/json_exporter.hpp>
#include <hpactor/tracing/otlp_exporter.hpp>
#include <hpactor/tracing/trace_context_parser.hpp>

#include <sstream>

namespace hpactor::tracing {

namespace {

std::string trace_id_hex(const TraceId& id) {
    TraceContext ctx;
    ctx.trace_id = id;
    ctx.span_id.bytes[7] = 1;
    return format_traceparent(ctx).substr(3, 32);
}

std::string span_id_hex(const SpanId& id) {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 1;
    ctx.span_id = id;
    return format_traceparent(ctx).substr(36, 16);
}

int otlp_span_kind(SpanKind kind) {
    switch (kind) {
        case SpanKind::kInternal:
            return 1;
        case SpanKind::kServer:
            return 2;
        case SpanKind::kClient:
            return 3;
        case SpanKind::kProducer:
            return 4;
        case SpanKind::kConsumer:
            return 5;
    }
    return 0;
}

} // namespace

OtlpHttpExporter::OtlpHttpExporter(std::string endpoint)
    : endpoint_(std::move(endpoint)) {}

std::string
OtlpHttpExporter::build_json_payload_for_test(std::span<const SpanRecord> batch,
                                              const std::string& service_name) const {
    std::ostringstream os;
    os << R"({"resourceSpans":[{"resource":{"attributes":[)"
       << R"({"key":"service.name","value":{"stringValue":")" << service_name
       << R"("}}]},"scopeSpans":[{"scope":{"name":"hpactor-native"},"spans":[)";
    for (size_t i = 0; i < batch.size(); ++i) {
        const auto& r = batch[i];
        if (i != 0)
            os << ',';
        os << R"({"traceId":")" << trace_id_hex(r.trace_id) << R"(","spanId":")"
           << span_id_hex(r.span_id) << R"(","name":"hpactor.span")"
           << R"(,"kind":)" << otlp_span_kind(r.kind)
           << R"(,"startTimeUnixNano":")" << r.start_ns << '"'
           << R"(,"endTimeUnixNano":")" << r.end_ns << '"' << R"(,"attributes":[)"
           << R"({"key":"hpactor.actor.id","value":{"intValue":")"
           << r.actor_id.value() << R"("}},)"
           << R"({"key":"hpactor.message.type_tag","value":{"intValue":")"
           << r.type_tag << R"("}}]})";
    }
    os << R"(]}]}]}]})";
    return os.str();
}

result<void>
OtlpHttpExporter::export_batch(std::span<const SpanRecord> batch) noexcept {
    if (batch.empty()) {
        return result<void>::make();
    }
    std::string body_text = build_json_payload_for_test(batch, "hpactor");
    StreamBuffer body;
    body.append(reinterpret_cast<const uint8_t*>(body_text.data()),
                body_text.size());

    hpactor::net::HttpClient client(nullptr);
    std::vector<hpactor::net::HttpHeader> headers = {
        {"content-type", "application/json"},
    };
    auto response =
        client.post(endpoint_, std::move(body), std::move(headers)).get();
    if (!response.has_value()) {
        return result<void>::make(
            error(response.error().code(), response.error().message()));
    }
    return result<void>::make();
}

} // namespace hpactor::tracing
