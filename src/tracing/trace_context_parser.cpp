#include <hpactor/tracing/trace_context_parser.hpp>

#include <array>
#include <cstring>

namespace hpactor::tracing {

namespace {

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

template <size_t N>
bool parse_hex_bytes(std::string_view text, std::array<uint8_t, N>& out) noexcept {
    if (text.size() != N * 2)
        return false;
    for (size_t i = 0; i < N; ++i) {
        int hi = hex_value(text[i * 2]);
        int lo = hex_value(text[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void append_hex_byte(std::string& out, uint8_t b) {
    constexpr char kHex[] = "0123456789abcdef";
    out.push_back(kHex[(b >> 4) & 0x0F]);
    out.push_back(kHex[b & 0x0F]);
}

} // namespace

TraceParseResult parse_w3c_trace_context(std::string_view traceparent,
                                         std::string_view tracestate,
                                         uint16_t max_tracestate_len) noexcept {
    TraceParseResult result;
    if (traceparent.empty()) {
        result.status_value = TraceParseStatus::kMissing;
        return result;
    }
    if (traceparent.size() != 55 || traceparent[2] != '-' ||
        traceparent[35] != '-' || traceparent[52] != '-') {
        result.status_value = TraceParseStatus::kMalformed;
        return result;
    }
    if (!traceparent.starts_with("00")) {
        result.status_value = TraceParseStatus::kUnsupportedVersion;
        return result;
    }

    TraceContext ctx;
    if (!parse_hex_bytes(traceparent.substr(3, 32), ctx.trace_id.bytes) ||
        !ctx.trace_id.valid()) {
        result.status_value = TraceParseStatus::kInvalidTraceId;
        return result;
    }
    if (!parse_hex_bytes(traceparent.substr(36, 16), ctx.span_id.bytes) ||
        !ctx.span_id.valid()) {
        result.status_value = TraceParseStatus::kInvalidSpanId;
        return result;
    }

    std::array<uint8_t, 1> flags{};
    if (!parse_hex_bytes(traceparent.substr(53, 2), flags)) {
        result.status_value = TraceParseStatus::kMalformed;
        return result;
    }
    ctx.flags.value = flags[0];

    if (tracestate.size() > max_tracestate_len ||
        tracestate.size() > ctx.tracestate.size()) {
        result.status_value = TraceParseStatus::kTracestateTooLarge;
        return result;
    }
    if (!tracestate.empty()) {
        std::memcpy(ctx.tracestate.data(), tracestate.data(), tracestate.size());
        ctx.tracestate_len = static_cast<uint16_t>(tracestate.size());
    }

    result.status_value = TraceParseStatus::kOk;
    result.context = ctx;
    return result;
}

std::string format_traceparent(const TraceContext& context) {
    std::string out;
    out.reserve(55);
    out += "00-";
    for (uint8_t b : context.trace_id.bytes)
        append_hex_byte(out, b);
    out.push_back('-');
    for (uint8_t b : context.span_id.bytes)
        append_hex_byte(out, b);
    out.push_back('-');
    append_hex_byte(out, context.flags.value);
    return out;
}

std::string format_tracestate(const TraceContext& context) {
    return std::string(context.tracestate_view());
}

} // namespace hpactor::tracing
