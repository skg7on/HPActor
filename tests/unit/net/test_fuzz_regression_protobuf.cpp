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

/// \file test_fuzz_regression_protobuf.cpp
/// \brief Regression tests for protobuf decode fuzz findings.
///
/// These tests replay known-edge-case protobuf payloads against
/// WireEnvelope, PbActorAddress, and PbTraceContext parsing to ensure
/// the decode paths never crash on adversarial input.

#include <gtest/gtest.h>
#include <hpactor/common.pb.h>
#include <hpactor/frame.pb.h>
#include <hpactor/msg/frame.hpp>
#include <string>

using namespace hpactor;

TEST(FuzzRegressionProtobuf, EmptyPayload) {
    // Empty bytes — should not parse, but must not crash
    std::string payload;
    net::WireEnvelope envelope;
    bool ok = envelope.ParseFromString(payload);
    // Protobuf accepts empty input as valid (no fields set)
    EXPECT_TRUE(ok);
    EXPECT_EQ(envelope.payload_case(), net::WireEnvelope::PAYLOAD_NOT_SET);
}

TEST(FuzzRegressionProtobuf, MaxVarint) {
    // 10 bytes of 0xFF — max varint, should not overflow
    std::string payload(10, '\xFF');
    net::WireEnvelope envelope;
    // Should not crash — parse may succeed or fail
    bool ok = envelope.ParseFromString(payload);
    (void)ok;
}

TEST(FuzzRegressionProtobuf, TruncatedFieldTag) {
    // Length-delimited field with huge declared length, no actual data
    // Wire type 2 (length-delimited), field 1 = tag 0x0a
    std::string payload("\x0a\x80\x80\x80\x80\x10", 6);
    net::WireEnvelope envelope;
    // Must not crash, even with truncated data
    bool ok = envelope.ParseFromString(payload);
    (void)ok;
}

TEST(FuzzRegressionProtobuf, AddressEmptyPayload) {
    // Parse empty bytes as PbActorAddress
    std::string payload;
    PbActorAddress addr;
    EXPECT_TRUE(addr.ParseFromString(payload));
    EXPECT_FALSE(addr.has_local_addr());
    EXPECT_FALSE(addr.has_global_addr());
}

TEST(FuzzRegressionProtobuf, TraceContextEmptyPayload) {
    // Parse empty bytes as PbTraceContext
    std::string payload;
    net::PbTraceContext trace;
    EXPECT_TRUE(trace.ParseFromString(payload));
    EXPECT_TRUE(trace.trace_id().empty());
    EXPECT_TRUE(trace.span_id().empty());
}

TEST(FuzzRegressionProtobuf, WireEnvelopeRoundTrip) {
    // Construct a valid DataFrame, serialize, re-parse, verify
    net::WireEnvelope original;
    auto* df = original.mutable_data_frame();
    df->set_type_tag(42);
    df->set_message_id(12345);
    df->set_flags(1 << 5); // AckRequested
    df->set_payload("test_payload", 12);
    df->mutable_sender()->mutable_local_addr()->set_actor_id(100);
    df->mutable_receiver()->mutable_local_addr()->set_actor_id(200);

    std::string serialized;
    ASSERT_TRUE(original.SerializeToString(&serialized));

    net::WireEnvelope reparsed;
    ASSERT_TRUE(reparsed.ParseFromString(serialized));

    EXPECT_EQ(reparsed.payload_case(), net::WireEnvelope::kDataFrame);
    EXPECT_EQ(reparsed.data_frame().type_tag(), 42u);
    EXPECT_EQ(reparsed.data_frame().message_id(), 12345u);
    EXPECT_EQ(reparsed.data_frame().payload(), "test_payload");
    EXPECT_TRUE(reparsed.data_frame().sender().has_local_addr());
    EXPECT_EQ(reparsed.data_frame().sender().local_addr().actor_id(), 100u);
}

TEST(FuzzRegressionProtobuf, DeeplyNestedRepeated) {
    // Adversarial repeated field — 10,000 entries with small payloads
    net::WireEnvelope envelope;
    auto* bf = envelope.mutable_batch_frame();
    for (int i = 0; i < 10000; ++i) {
        auto* entry = bf->add_entries();
        entry->set_type_tag(1);
        entry->set_message_id(static_cast<uint64_t>(i));
    }
    std::string serialized;
    ASSERT_TRUE(envelope.SerializeToString(&serialized));

    // Reparse — must not crash despite large repeated field
    net::WireEnvelope reparsed;
    ASSERT_TRUE(reparsed.ParseFromString(serialized));
    EXPECT_EQ(reparsed.payload_case(), net::WireEnvelope::kBatchFrame);
    EXPECT_GE(reparsed.batch_frame().entries_size(), 10000);
}
