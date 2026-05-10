#include <hpactor/actor/typed_message.hpp>

#include <cassert>

using namespace hpactor;

static TraceContext make_context() {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 1;
    ctx.span_id.bytes[7] = 2;
    ctx.flags.set_sampled(true);
    return ctx;
}

int main() {
    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    assert(!msg.has_trace_context());

    TraceContext ctx = make_context();
    msg.set_trace_context(ctx);
    assert(msg.has_trace_context());
    assert(msg.trace_context().trace_id == ctx.trace_id);
    assert(msg.trace_context().span_id == ctx.span_id);
    assert(msg.trace_context().sampled());

    TypedMessage moved(std::move(msg));
    assert(moved.has_trace_context());
    assert(moved.trace_context().trace_id == ctx.trace_id);

    TypedMessage assigned(TypeTag::User, StreamBuffer{});
    assigned = std::move(moved);
    assert(assigned.has_trace_context());
    assert(assigned.trace_context().span_id == ctx.span_id);

    assigned.clear_trace_context();
    assert(!assigned.has_trace_context());
    assert(!assigned.trace_context().valid());
    return 0;
}
