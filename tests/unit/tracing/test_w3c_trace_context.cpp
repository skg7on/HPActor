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
#include <hpactor/tracing/trace_context_parser.hpp>

using namespace hpactor;
using namespace hpactor::tracing;

TEST(W3CTraceContextTest, ParseValid) {
    auto ok = parse_w3c_trace_context("00-4bf92f3577b34da6a3ce929d0e0e4736-"
                                      "00f067aa0ba902b7-01",
                                      "vendor=value", 256);
    EXPECT_EQ(ok.status_value, TraceParseStatus::kOk);
    EXPECT_TRUE(ok.context.valid());
    EXPECT_TRUE(ok.context.sampled());
    EXPECT_EQ(format_traceparent(ok.context), "00-"
                                              "4bf92f3577b34da6a3ce929d0e0e4736"
                                              "-"
                                              "00f067aa0ba902b7-01");
    EXPECT_EQ(format_tracestate(ok.context), "vendor=value");
}

TEST(W3CTraceContextTest, ParseUppercase) {
    auto uppercase = parse_w3c_trace_context("00-"
                                             "4BF92F3577B34DA6A3CE929D0E0E4736-"
                                             "00F067AA0BA902B7-00",
                                             "", 256);
    EXPECT_EQ(uppercase.status_value, TraceParseStatus::kOk);
    EXPECT_FALSE(uppercase.context.sampled());
    EXPECT_EQ(format_traceparent(uppercase.context), "00-"
                                                     "4bf92f3577b34da6a3ce929d0"
                                                     "e"
                                                     "0e4736-00f067aa0ba902b7-"
                                                     "00");
}

TEST(W3CTraceContextTest, ParseMissing) {
    auto missing = parse_w3c_trace_context("", "", 256);
    EXPECT_EQ(missing.status_value, TraceParseStatus::kMissing);
}

TEST(W3CTraceContextTest, ParseZeroTraceId) {
    auto zero_trace = parse_w3c_trace_context("00-"
                                              "00000000000000000000000000000000"
                                              "-00f067aa0ba902b7-01",
                                              "", 256);
    EXPECT_EQ(zero_trace.status_value, TraceParseStatus::kInvalidTraceId);
}

TEST(W3CTraceContextTest, ParseZeroSpanId) {
    auto zero_span = parse_w3c_trace_context("00-"
                                             "4bf92f3577b34da6a3ce929d0e0e4736-"
                                             "0000000000000000-01",
                                             "", 256);
    EXPECT_EQ(zero_span.status_value, TraceParseStatus::kInvalidSpanId);
}

TEST(W3CTraceContextTest, ParseMalformed) {
    auto malformed = parse_w3c_trace_context("00-short", "", 256);
    EXPECT_EQ(malformed.status_value, TraceParseStatus::kMalformed);
}

TEST(W3CTraceContextTest, TracestateTooLarge) {
    auto too_large_state = parse_w3c_trace_context("00-"
                                                   "4bf92f3577b34da6a3ce929d0e0"
                                                   "e4736-00f067aa0ba902b7-01",
                                                   "abcdefgh", 4);
    EXPECT_EQ(too_large_state.status_value, TraceParseStatus::kTracestateTooLarge);
}
