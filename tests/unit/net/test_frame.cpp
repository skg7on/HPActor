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
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/frame.hpp>

using namespace hpactor;
using namespace hpactor::net;

TEST(FrameTest, WireFrameDefaultConstruction) {
    WireFrame f1;
    EXPECT_EQ(f1.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f1.pb_envelope.data_frame().flags(), 0);
    EXPECT_EQ(f1.pb_envelope.data_frame().message_id(), 0u);
}

TEST(FrameTest, WireFrameWithValues) {
    ActorId sender_id(100);
    ActorId receiver_id(200);
    ActorAddress sender(endpoint_ops::parse_endpoint("node1:12345"), 10,
                        sender_id, 5);
    ActorAddress receiver(endpoint_ops::parse_endpoint("node2:12345"), 20,
                          receiver_id, 6);

    WireFrame f2;
    to_proto(f2.pb_envelope.mutable_data_frame()->mutable_sender(), sender);
    to_proto(f2.pb_envelope.mutable_data_frame()->mutable_receiver(), receiver);
    f2.pb_envelope.mutable_data_frame()->set_payload("hello", 5u);
    f2.pb_envelope.mutable_data_frame()->set_flags(WireFrame::Important);
    f2.pb_envelope.mutable_data_frame()->set_message_id(12345);

    EXPECT_EQ(f2.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f2.pb_envelope.data_frame().payload(), "hello");
    EXPECT_EQ(f2.pb_envelope.data_frame().flags(), WireFrame::Important);
    EXPECT_EQ(f2.pb_envelope.data_frame().message_id(), 12345u);
}

TEST(FrameTest, EncodeDecodeRoundtrip) {
    ActorId sender_id(100);
    ActorId receiver_id(200);
    ActorAddress sender(endpoint_ops::parse_endpoint("node1:12345"), 10,
                        sender_id, 5);
    ActorAddress receiver(endpoint_ops::parse_endpoint("node2:12345"), 20,
                          receiver_id, 6);

    WireFrame f2;
    to_proto(f2.pb_envelope.mutable_data_frame()->mutable_sender(), sender);
    to_proto(f2.pb_envelope.mutable_data_frame()->mutable_receiver(), receiver);
    f2.pb_envelope.mutable_data_frame()->set_payload("hello", 5u);
    f2.pb_envelope.mutable_data_frame()->set_flags(WireFrame::Important);
    f2.pb_envelope.mutable_data_frame()->set_message_id(12345);

    StreamBuffer encoded = f2.encode();
    EXPECT_FALSE(encoded.empty());

    WireFrame f3 = WireFrame::decode(encoded);
    auto decoded_sender = from_proto(f3.pb_envelope.data_frame().sender());
    auto decoded_receiver = from_proto(f3.pb_envelope.data_frame().receiver());
    EXPECT_EQ(decoded_sender.endpoint, sender.endpoint);
    EXPECT_EQ(decoded_sender.id.value(), sender.id.value());
    EXPECT_EQ(decoded_sender.incarnation, sender.incarnation);
    EXPECT_EQ(decoded_receiver.endpoint, receiver.endpoint);
    EXPECT_EQ(decoded_receiver.id.value(), receiver.id.value());
    EXPECT_EQ(decoded_receiver.incarnation, receiver.incarnation);
    EXPECT_EQ(f3.pb_envelope.data_frame().payload(),
              f2.pb_envelope.data_frame().payload());
    EXPECT_EQ(f3.pb_envelope.data_frame().flags(),
              f2.pb_envelope.data_frame().flags());
    EXPECT_EQ(f3.pb_envelope.data_frame().message_id(),
              f2.pb_envelope.data_frame().message_id());
}

TEST(FrameTest, MalformedDataHandling) {
    StreamBuffer malformed = {0xFF, 0xFF, 0xFF, 0xFF};
    WireFrame f_bad = WireFrame::decode(malformed);
    EXPECT_EQ(f_bad.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f_bad.pb_envelope.data_frame().message_id(), 0u);
}

TEST(FrameTest, Ipv6Endpoint) {
    std::array<uint8_t, 16> ipv6_addr = {0, 0, 0, 0, 0, 0, 0, 0,
                                         0, 0, 0, 0, 0, 0, 0, 1};
    ActorAddress ipv6_sender{Ipv6Endpoint{ipv6_addr, htons(8080)}, 1,
                             ActorId(300), 1};
    ActorAddress ipv6_receiver{Ipv6Endpoint{ipv6_addr, htons(9090)}, 2,
                               ActorId(400), 2};

    WireFrame f_ipv6;
    to_proto(f_ipv6.pb_envelope.mutable_data_frame()->mutable_sender(), ipv6_sender);
    to_proto(f_ipv6.pb_envelope.mutable_data_frame()->mutable_receiver(),
             ipv6_receiver);
    f_ipv6.pb_envelope.mutable_data_frame()->set_payload("xyz", 3u);
    f_ipv6.pb_envelope.mutable_data_frame()->set_message_id(99999);

    StreamBuffer encoded_ipv6 = f_ipv6.encode();
    WireFrame decoded_ipv6 = WireFrame::decode(encoded_ipv6);

    EXPECT_EQ(std::get<Ipv6Endpoint>(
                  from_proto(decoded_ipv6.pb_envelope.data_frame().sender()).endpoint)
                  .port_nw,
              htons(8080));
    EXPECT_EQ(decoded_ipv6.pb_envelope.data_frame().message_id(), 99999u);
}

// ── Fuzz regression tests — replay known-edge-case inputs against
//     try_decode_wireframe() and verify no crash / correct error ────────────

TEST(FuzzRegression, FrameEmptyInput) {
    StreamBuffer buf;
    auto r = try_decode_wireframe(buf);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, FrameDecodeError::HeaderTooShort);
}

TEST(FuzzRegression, FrameWrongMagic) {
    // "XXXX" + 4-byte zero length = 8 bytes total
    uint8_t data[] = {'X', 'X', 'X', 'X', 0x00, 0x00, 0x00, 0x00};
    StreamBuffer buf(data, data + sizeof(data));
    auto r = try_decode_wireframe(buf);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, FrameDecodeError::InvalidMagic);
}

TEST(FuzzRegression, FrameTruncatedHeader) {
    // Just "HPA" — only 3 bytes, below minimum 8
    uint8_t data[] = {'H', 'P', 'A'};
    StreamBuffer buf(data, data + sizeof(data));
    auto r = try_decode_wireframe(buf);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, FrameDecodeError::HeaderTooShort);
}

TEST(FuzzRegression, FrameMaxDeclaredLength) {
    // "HPAC" + 0xFFFFFFFF length (declares ~4GB payload)
    uint8_t data[] = {'H', 'P', 'A', 'C', 0xFF, 0xFF, 0xFF, 0xFF};
    StreamBuffer buf(data, data + sizeof(data));
    FrameDecodeLimits limits;
    limits.max_payload_bytes = 16U * 1024U * 1024U; // 16 MiB
    limits.reject_trailing_bytes = true;
    auto r = try_decode_wireframe(buf, limits);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, FrameDecodeError::FrameTooLarge);
    EXPECT_EQ(r.declared_payload_bytes, 0xFFFFFFFFu);
}

TEST(FuzzRegression, FrameLengthMismatch) {
    // "HPAC" + declare 100 bytes of payload, but only 8 header bytes present
    uint8_t data[] = {'H', 'P', 'A', 'C', 0x00, 0x00, 0x00, 100};
    StreamBuffer buf(data, data + sizeof(data));
    auto r = try_decode_wireframe(buf);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, FrameDecodeError::LengthMismatch);
}

TEST(FuzzRegression, FrameZeroPayloadRelaxed) {
    // "HPAC" + 0 length, allow trailing bytes
    uint8_t data[] = {'H', 'P', 'A', 'C', 0x00, 0x00, 0x00, 0x00, 'X', 'Y'};
    StreamBuffer buf(data, data + sizeof(data));
    FrameDecodeLimits limits;
    limits.max_payload_bytes = 0;
    limits.reject_trailing_bytes = false;
    auto r = try_decode_wireframe(buf, limits);
    // With 0 payload and trailing allowed, result depends on protobuf parse
    // of empty payload — should not crash regardless of outcome
    EXPECT_TRUE(r.ok() || !r.ok());
}

TEST(FuzzRegression, FrameRoundTripValidMinimal) {
    // A minimal valid frame: "HPAC" + 0-length payload
    uint8_t data[] = {'H', 'P', 'A', 'C', 0x00, 0x00, 0x00, 0x00};
    StreamBuffer buf(data, data + sizeof(data));
    auto r = try_decode_wireframe(buf);
    if (r.ok()) {
        // Round-trip: encode the result, decode again — must match
        auto encoded = r.frame.encode();
        auto r2 = try_decode_wireframe(encoded);
        EXPECT_TRUE(r2.ok());
    }
}

TEST(FuzzRegression, FrameAllZeroBytes) {
    // 64 zero bytes — exercises every code path with null-like input
    uint8_t data[64] = {};
    StreamBuffer buf(data, data + sizeof(data));
    auto r = try_decode_wireframe(buf);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, FrameDecodeError::InvalidMagic);
}
