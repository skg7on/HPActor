#include <hpactor/net/frame.hpp>

#include <cassert>

using namespace hpactor;

static TraceContext make_context() {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 9;
    ctx.span_id.bytes[7] = 8;
    ctx.flags.set_sampled(true);
    return ctx;
}

int main() {
    TraceContext ctx = make_context();
    hpactor::net::WireFrame frame;
    hpactor::net::to_proto(frame.pb_frame.mutable_trace_context(), ctx);
    assert(frame.pb_frame.has_trace_context());

    auto parsed =
        hpactor::net::trace_context_from_proto(frame.pb_frame.trace_context(), 256);
    assert(parsed.has_value());
    assert(parsed.value().trace_id == ctx.trace_id);
    assert(parsed.value().span_id == ctx.span_id);
    assert(parsed.value().sampled());

    auto encoded = frame.encode();
    auto decoded = hpactor::net::WireFrame::decode(encoded);
    assert(decoded.pb_frame.has_trace_context());
    return 0;
}
