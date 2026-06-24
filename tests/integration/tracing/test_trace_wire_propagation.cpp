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

#include <hpactor/msg/frame.hpp>

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

} // namespace

TEST(TraceWirePropagationTest, RoundTripViaProtobufWireFrame) {
    TraceContext ctx = make_context();
    hpactor::net::WireFrame frame;
    hpactor::net::to_proto(
        frame.pb_envelope.mutable_data_frame()->mutable_trace_context(), ctx);
    ASSERT_TRUE(frame.pb_envelope.data_frame().has_trace_context());

    auto parsed = hpactor::net::trace_context_from_proto(
        frame.pb_envelope.data_frame().trace_context(), 256);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().trace_id, ctx.trace_id);
    EXPECT_EQ(parsed.value().span_id, ctx.span_id);
    EXPECT_TRUE(parsed.value().sampled());

    auto encoded = frame.encode();
    auto decoded = hpactor::net::WireFrame::decode(encoded);
    ASSERT_TRUE(decoded.pb_envelope.data_frame().has_trace_context());
}