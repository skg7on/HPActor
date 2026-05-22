// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/net/frame.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

namespace {

TraceContext make_context() {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 9;
    ctx.span_id.bytes[7] = 8;
    ctx.flags.set_sampled(true);
    return ctx;
}

}  // namespace

TEST(TraceWirePropagationTest, RoundTripViaProtobufWireFrame) {
    TraceContext ctx = make_context();
    hpactor::net::WireFrame frame;
    hpactor::net::to_proto(frame.pb_frame.mutable_trace_context(), ctx);
    ASSERT_TRUE(frame.pb_frame.has_trace_context());

    auto parsed =
        hpactor::net::trace_context_from_proto(frame.pb_frame.trace_context(), 256);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().trace_id, ctx.trace_id);
    EXPECT_EQ(parsed.value().span_id, ctx.span_id);
    EXPECT_TRUE(parsed.value().sampled());

    auto encoded = frame.encode();
    auto decoded = hpactor::net::WireFrame::decode(encoded);
    ASSERT_TRUE(decoded.pb_frame.has_trace_context());
}
