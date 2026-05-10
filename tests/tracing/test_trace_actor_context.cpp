#include <hpactor/actor_context.hpp>

#include <cassert>

using namespace hpactor;

static TraceContext make_context() {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 3;
    ctx.span_id.bytes[7] = 4;
    return ctx;
}

int main() {
    ActorContext ctx(Actor{});
    assert(!ctx.has_current_trace_context());
    TraceContext trace = make_context();
    {
        ActorContext::TraceScope scope(&ctx, trace);
        assert(ctx.has_current_trace_context());
        assert(ctx.current_trace_context().trace_id == trace.trace_id);
    }
    assert(!ctx.has_current_trace_context());
    return 0;
}
