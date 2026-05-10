#pragma once

#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace hpactor::tracing {

enum class TraceParseStatus : uint8_t {
    kOk,
    kMissing,
    kMalformed,
    kUnsupportedVersion,
    kInvalidTraceId,
    kInvalidSpanId,
    kTracestateTooLarge,
};

struct TraceParseResult {
    TraceParseStatus status{TraceParseStatus::kMissing};
    TraceContext context{};
};

TraceParseResult parse_w3c_trace_context(std::string_view traceparent,
                                         std::string_view tracestate,
                                         uint16_t max_tracestate_len) noexcept;

std::string format_traceparent(const TraceContext& context);
std::string format_tracestate(const TraceContext& context);

} // namespace hpactor::tracing
