#include <hpactor/tracing/trace_context_parser.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::tracing;

int main() {
    auto ok = parse_w3c_trace_context("00-4bf92f3577b34da6a3ce929d0e0e4736-"
                                      "00f067aa0ba902b7-01",
                                      "vendor=value", 256);
    assert(ok.status_value == TraceParseStatus::kOk);
    assert(ok.context.valid());
    assert(ok.context.sampled());
    assert(format_traceparent(ok.context) == "00-"
                                             "4bf92f3577b34da6a3ce929d0e0e4736-"
                                             "00f067aa0ba902b7-01");
    assert(format_tracestate(ok.context) == "vendor=value");

    auto uppercase = parse_w3c_trace_context("00-"
                                             "4BF92F3577B34DA6A3CE929D0E0E4736-"
                                             "00F067AA0BA902B7-00",
                                             "", 256);
    assert(uppercase.status_value == TraceParseStatus::kOk);
    assert(!uppercase.context.sampled());
    assert(format_traceparent(uppercase.context) == "00-"
                                                    "4bf92f3577b34da6a3ce929d0e"
                                                    "0e4736-00f067aa0ba902b7-"
                                                    "00");

    auto missing = parse_w3c_trace_context("", "", 256);
    assert(missing.status_value == TraceParseStatus::kMissing);

    auto zero_trace = parse_w3c_trace_context("00-"
                                              "00000000000000000000000000000000"
                                              "-00f067aa0ba902b7-01",
                                              "", 256);
    assert(zero_trace.status_value == TraceParseStatus::kInvalidTraceId);

    auto zero_span = parse_w3c_trace_context("00-"
                                             "4bf92f3577b34da6a3ce929d0e0e4736-"
                                             "0000000000000000-01",
                                             "", 256);
    assert(zero_span.status_value == TraceParseStatus::kInvalidSpanId);

    auto malformed = parse_w3c_trace_context("00-short", "", 256);
    assert(malformed.status_value == TraceParseStatus::kMalformed);

    auto too_large_state = parse_w3c_trace_context("00-"
                                                   "4bf92f3577b34da6a3ce929d0e0"
                                                   "e4736-00f067aa0ba902b7-01",
                                                   "abcdefgh", 4);
    assert(too_large_state.status_value == TraceParseStatus::kTracestateTooLarge);
    return 0;
}
