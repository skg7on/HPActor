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
#include <hpactor/common.pb.h>
#include <hpactor/frame.pb.h>

using namespace hpactor;
using namespace hpactor::net;

namespace {
void set_local_sender(::hpactor::PbActorAddress* addr, uint64_t actor_id,
                      uint32_t actor_type = 1) {
    auto* local = addr->mutable_local_addr();
    local->set_actor_id(actor_id);
    local->set_actor_type(actor_type);
}
} // namespace

// ── AckFrame encode/decode ─────────────────────────────────────────────────

TEST(AckFrameTest, RoundTrip) {
    AckFrame original;
    original.set_message_id(12345);
    set_local_sender(original.mutable_sender(), 7);
    original.set_sender_node_id(42);

    std::string wire;
    ASSERT_TRUE(original.SerializeToString(&wire));

    AckFrame decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.message_id(), 12345);
    EXPECT_TRUE(decoded.sender().has_local_addr());
    EXPECT_EQ(decoded.sender().local_addr().actor_id(), 7);
    EXPECT_EQ(decoded.sender_node_id(), 42);
}

// ── NackFrame encode/decode ────────────────────────────────────────────────

TEST(NackFrameTest, RoundTripMailboxFull) {
    NackFrame original;
    original.set_message_id(999);
    set_local_sender(original.mutable_sender(), 2);
    original.set_sender_node_id(1);
    original.set_reason(NackReason::NACK_MAILBOX_FULL);
    original.set_retry_after_ms(500);

    std::string wire;
    ASSERT_TRUE(original.SerializeToString(&wire));

    NackFrame decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.message_id(), 999);
    EXPECT_EQ(decoded.reason(), NackReason::NACK_MAILBOX_FULL);
    EXPECT_EQ(decoded.retry_after_ms(), 500);
}

TEST(NackFrameTest, RoundTripActorDead) {
    NackFrame original;
    original.set_message_id(1);
    set_local_sender(original.mutable_sender(), 4);
    original.set_sender_node_id(3);
    original.set_reason(NackReason::NACK_ACTOR_DEAD);

    std::string wire;
    ASSERT_TRUE(original.SerializeToString(&wire));

    NackFrame decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.message_id(), 1);
    EXPECT_EQ(decoded.reason(), NackReason::NACK_ACTOR_DEAD);
}

// ── WireEnvelope dispatch ──────────────────────────────────────────────────

TEST(WireEnvelopeTest, DataFrameVariant) {
    WireEnvelope env;
    auto* df = env.mutable_data_frame();
    df->set_message_id(42);
    df->set_type_tag(100);

    std::string wire;
    ASSERT_TRUE(env.SerializeToString(&wire));

    WireEnvelope decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.payload_case(), WireEnvelope::kDataFrame);
    EXPECT_EQ(decoded.data_frame().message_id(), 42);
    EXPECT_EQ(decoded.data_frame().type_tag(), 100);
}

TEST(WireEnvelopeTest, AckFrameVariant) {
    WireEnvelope env;
    auto* af = env.mutable_ack_frame();
    af->set_message_id(77);

    std::string wire;
    ASSERT_TRUE(env.SerializeToString(&wire));

    WireEnvelope decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.payload_case(), WireEnvelope::kAckFrame);
    EXPECT_EQ(decoded.ack_frame().message_id(), 77);
}

TEST(WireEnvelopeTest, NackFrameVariant) {
    WireEnvelope env;
    auto* nf = env.mutable_nack_frame();
    nf->set_message_id(88);
    nf->set_reason(NackReason::NACK_MAILBOX_FULL);
    nf->set_retry_after_ms(200);

    std::string wire;
    ASSERT_TRUE(env.SerializeToString(&wire));

    WireEnvelope decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.payload_case(), WireEnvelope::kNackFrame);
    EXPECT_EQ(decoded.nack_frame().message_id(), 88);
    EXPECT_EQ(decoded.nack_frame().reason(), NackReason::NACK_MAILBOX_FULL);
    EXPECT_EQ(decoded.nack_frame().retry_after_ms(), 200);
}
