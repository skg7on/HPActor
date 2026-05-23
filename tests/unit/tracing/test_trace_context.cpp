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

#include <cstring>
#include <gtest/gtest.h>
#include <hpactor/types/types.hpp>

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

TEST(TraceContextTest, EmptyTrace) {
    TraceId empty_trace;
    SpanId empty_span;
    EXPECT_FALSE(empty_trace.valid());
    EXPECT_FALSE(empty_span.valid());
}

TEST(TraceContextTest, EmptyContext) {
    TraceContext empty;
    EXPECT_FALSE(empty.valid());
    EXPECT_FALSE(empty.sampled());
}

TEST(TraceContextTest, ValidContext) {
    TraceContext ctx = make_context();
    EXPECT_TRUE(ctx.valid());
    EXPECT_TRUE(ctx.sampled());
    EXPECT_EQ(ctx.tracestate_view(), "vendor=value");
}

TEST(TraceContextTest, ToggleSampled) {
    TraceContext ctx = make_context();
    ctx.flags.set_sampled(false);
    EXPECT_FALSE(ctx.sampled());
}

TEST(TraceContextTest, Clear) {
    TraceContext ctx = make_context();
    ctx.clear();
    EXPECT_FALSE(ctx.valid());
    EXPECT_EQ(ctx.tracestate_len, 0u);
}
