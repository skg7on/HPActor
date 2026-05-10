#include <hpactor/types/types.hpp>

#include <cassert>
#include <cstring>

using namespace hpactor;

static TraceContext make_context() {
    TraceContext ctx;
    for (uint8_t i = 0; i < ctx.trace_id.bytes.size(); ++i) {
        ctx.trace_id.bytes[i] = static_cast<uint8_t>(i + 1);
    }
    for (uint8_t i = 0; i < ctx.span_id.bytes.size(); ++i) {
        ctx.span_id.bytes[i] = static_cast<uint8_t>(0xA0 + i);
    }
    ctx.flags.set_sampled(true);
    const char state[] = "vendor=value";
    std::memcpy(ctx.tracestate.data(), state, sizeof(state) - 1);
    ctx.tracestate_len = sizeof(state) - 1;
    return ctx;
}

int main() {
    TraceId empty_trace;
    SpanId empty_span;
    assert(!empty_trace.valid());
    assert(!empty_span.valid());

    TraceContext empty;
    assert(!empty.valid());
    assert(!empty.sampled());

    TraceContext ctx = make_context();
    assert(ctx.valid());
    assert(ctx.sampled());
    assert(ctx.tracestate_view() == "vendor=value");

    ctx.flags.set_sampled(false);
    assert(!ctx.sampled());

    ctx.clear();
    assert(!ctx.valid());
    assert(ctx.tracestate_len == 0);
    return 0;
}
