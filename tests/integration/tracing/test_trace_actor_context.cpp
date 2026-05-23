#include <gtest/gtest.h>

#include <hpactor/actor_context.hpp>

using namespace hpactor;

static TraceContext make_context() {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 3;
    ctx.span_id.bytes[7] = 4;
    return ctx;
}

TEST(TraceActorContextTest, NoTraceContextInitially) {
    ActorContext ctx(Actor{});
    EXPECT_FALSE(ctx.has_current_trace_context());
}

TEST(TraceActorContextTest, TraceScopeSetsAndClearsContext) {
    ActorContext ctx(Actor{});
    EXPECT_FALSE(ctx.has_current_trace_context());

    TraceContext trace = make_context();
    {
        ActorContext::TraceScope scope(&ctx, trace);
        EXPECT_TRUE(ctx.has_current_trace_context());
        EXPECT_EQ(ctx.current_trace_context().trace_id, trace.trace_id);
    }

    EXPECT_FALSE(ctx.has_current_trace_context());
}
