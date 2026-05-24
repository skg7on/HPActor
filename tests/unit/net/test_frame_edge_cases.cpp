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
#include <hpactor/net/frame.hpp>

using namespace hpactor;
using namespace hpactor::net;

TEST(FrameEdgeTest, DecodeEmptyBuffer) {
    StreamBuffer empty;
    WireFrame f = WireFrame::decode(empty);
    EXPECT_EQ(f.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f.pb_frame.message_id(), 0u);
}

TEST(FrameEdgeTest, DecodeTruncatedHeader) {
    StreamBuffer short_data = {'H', 'P', 'A'};
    WireFrame f = WireFrame::decode(short_data);
    EXPECT_EQ(f.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f.pb_frame.message_id(), 0u);
}

TEST(FrameEdgeTest, DecodeWrongMagic) {
    StreamBuffer bad_magic = {'B', 'A', 'D', '!', 0, 0, 0, 10, 1,
                              2,   3,   4,   5,   6, 7, 8, 9,  0};
    WireFrame f = WireFrame::decode(bad_magic);
    EXPECT_EQ(f.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f.pb_frame.message_id(), 0u);
}

TEST(FrameEdgeTest, DecodeTruncatedPayload) {
    // Valid magic + length field saying payload is 100 bytes, but only 4 bytes
    // follow
    StreamBuffer truncated;
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'C'};
    truncated.append(magic.data(), 4);
    uint32_t big_len = htonl(100u);
    truncated.append(reinterpret_cast<const uint8_t*>(&big_len), 4u);
    truncated.append(reinterpret_cast<const uint8_t*>("abcd"), 4u);
    WireFrame f = WireFrame::decode(truncated);
    EXPECT_EQ(f.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f.pb_frame.message_id(), 0u);
}

TEST(FrameEdgeTest, DecodeGarbageProtobuf) {
    // Valid magic + valid length, but garbage payload (not valid protobuf)
    StreamBuffer garbage;
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'C'};
    garbage.append(magic.data(), 4);
    // Payload of 20 bytes of garbage
    uint32_t payload_len = htonl(20u);
    garbage.append(reinterpret_cast<const uint8_t*>(&payload_len), 4u);
    for (size_t i = 0; i < 20; ++i)
        garbage.push_back(static_cast<uint8_t>(0xFF));
    WireFrame f = WireFrame::decode(garbage);
    EXPECT_EQ(f.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f.pb_frame.message_id(), 0u);
}

TEST(FrameEdgeTest, DecodeSpanOverload) {
    // Test the span overload delegates to StreamBuffer version correctly
    ActorId sender_id(1);
    ActorId receiver_id(2);
    ActorAddress sender(endpoint_ops::parse_endpoint(""), 1, sender_id, 1);
    ActorAddress receiver(endpoint_ops::parse_endpoint(""), 2, receiver_id, 1);

    WireFrame original;
    to_proto(original.pb_frame.mutable_sender(), sender);
    to_proto(original.pb_frame.mutable_receiver(), receiver);
    original.pb_frame.set_payload("test", 4u);
    original.pb_frame.set_message_id(42u);

    StreamBuffer encoded = original.encode();
    // Decode via span overload
    std::span<const uint8_t> span(encoded.data(), encoded.size());
    WireFrame decoded = WireFrame::decode(span);
    EXPECT_EQ(decoded.pb_frame.message_id(), 42u);
    EXPECT_EQ(decoded.pb_frame.payload(), "test");
}

TEST(FrameEdgeTest, DecodeExactBoundaryPayload) {
    // Valid magic + length where payload exactly fills the remaining buffer
    ActorId sender_id(1);
    ActorId receiver_id(2);
    ActorAddress sender(endpoint_ops::parse_endpoint(""), 1, sender_id, 1);
    ActorAddress receiver(endpoint_ops::parse_endpoint(""), 2, receiver_id, 1);

    WireFrame original;
    to_proto(original.pb_frame.mutable_sender(), sender);
    to_proto(original.pb_frame.mutable_receiver(), receiver);
    original.pb_frame.set_payload("exact", 5u);
    original.pb_frame.set_message_id(99u);

    StreamBuffer encoded = original.encode();
    WireFrame decoded = WireFrame::decode(encoded);
    EXPECT_EQ(decoded.pb_frame.message_id(), 99u);
    EXPECT_EQ(decoded.pb_frame.payload(), "exact");
}

TEST(FrameEdgeTest, DecodeZeroLengthPayload) {
    // Frame with zero-length payload (valid protobuf with no fields set)
    StreamBuffer zero_payload;
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'C'};
    zero_payload.append(magic.data(), 4);
    uint32_t zero_len = htonl(0u);
    zero_payload.append(reinterpret_cast<const uint8_t*>(&zero_len), 4u);
    WireFrame f = WireFrame::decode(zero_payload);
    EXPECT_EQ(f.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f.pb_frame.message_id(), 0u);
    EXPECT_EQ(f.pb_frame.payload(), "");
}

TEST(FrameEdgeTest, EncodeEmptyPbFrame) {
    WireFrame f;
    StreamBuffer encoded = f.encode();
    // Should have valid magic header + length prefix
    EXPECT_GE(encoded.size(), WireFrame::HeaderSize);
    // Verify magic
    EXPECT_EQ(encoded[0], 'H');
    EXPECT_EQ(encoded[1], 'P');
    EXPECT_EQ(encoded[2], 'A');
    EXPECT_EQ(encoded[3], 'C');
    // Should roundtrip
    WireFrame decoded = WireFrame::decode(encoded);
    EXPECT_EQ(decoded.pb_frame.message_id(), 0u);
}
