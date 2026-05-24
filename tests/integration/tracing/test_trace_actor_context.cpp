// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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