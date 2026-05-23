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
#include <hpactor/actor/typed_message.hpp>

using namespace hpactor;

static TraceContext make_context() {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 1;
    ctx.span_id.bytes[7] = 2;
    ctx.flags.set_sampled(true);
    return ctx;
}

TEST(TraceTypedMessageTest, NoTraceContextByDefault) {
    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    EXPECT_FALSE(msg.has_trace_context());
}

TEST(TraceTypedMessageTest, SetAndGetTraceContext) {
    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    TraceContext ctx = make_context();
    msg.set_trace_context(ctx);
    EXPECT_TRUE(msg.has_trace_context());
    EXPECT_EQ(msg.trace_context().trace_id, ctx.trace_id);
    EXPECT_EQ(msg.trace_context().span_id, ctx.span_id);
    EXPECT_TRUE(msg.trace_context().sampled());
}

TEST(TraceTypedMessageTest, MoveConstructorPreservesTraceContext) {
    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    TraceContext ctx = make_context();
    msg.set_trace_context(ctx);

    TypedMessage moved(std::move(msg));
    EXPECT_TRUE(moved.has_trace_context());
    EXPECT_EQ(moved.trace_context().trace_id, ctx.trace_id);
}

TEST(TraceTypedMessageTest, MoveAssignmentPreservesTraceContext) {
    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    TraceContext ctx = make_context();
    msg.set_trace_context(ctx);

    TypedMessage assigned(TypeTag::User, StreamBuffer{});
    assigned = std::move(msg);
    EXPECT_TRUE(assigned.has_trace_context());
    EXPECT_EQ(assigned.trace_context().span_id, ctx.span_id);
}

TEST(TraceTypedMessageTest, ClearTraceContext) {
    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    msg.set_trace_context(make_context());
    msg.clear_trace_context();
    EXPECT_FALSE(msg.has_trace_context());
    EXPECT_FALSE(msg.trace_context().valid());
}
