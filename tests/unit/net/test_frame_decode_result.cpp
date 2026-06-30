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
#include <hpactor/net/frame_dispatch_result.hpp>

#include <gtest/gtest.h>

#include <arpa/inet.h>

namespace hpactor::net {
namespace {

// ── Helper: make a valid empty frame ─────────────────────────────────────

StreamBuffer make_empty_frame_bytes() {
    // Build a valid WireFrame with no payload fields (Unknown oneof)
    WireFrame empty;
    StreamBuffer encoded = empty.encode();
    return encoded;
}

// ── FrameDecodeResult test suite ──────────────────────────────────────────

class FrameDecodeResultTest : public ::testing::Test {
  protected:
    FrameDecodeLimits limits{};
};

TEST_F(FrameDecodeResultTest, ExactSuccess) {
    auto bytes = make_empty_frame_bytes();
    auto result = try_decode_wireframe(bytes, limits);
    EXPECT_EQ(result.error, FrameDecodeError::None);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.declared_payload_bytes, 0u);
}

TEST_F(FrameDecodeResultTest, HeaderTooShort) {
    StreamBuffer tiny;
    tiny.append(std::array<uint8_t, 3>{'H', 'P', 'A'}.data(), 3);
    auto result = try_decode_wireframe(tiny, limits);
    EXPECT_EQ(result.error, FrameDecodeError::HeaderTooShort);
    EXPECT_FALSE(result.ok());
}

TEST_F(FrameDecodeResultTest, InvalidMagic) {
    StreamBuffer bad;
    const std::array<uint8_t, 4> bad_magic = {'X', 'X', 'X', 'X'};
    bad.append(bad_magic.data(), 4);
    uint32_t net_len = htonl(0u);
    bad.append(reinterpret_cast<const uint8_t*>(&net_len), 4);
    auto result = try_decode_wireframe(bad, limits);
    EXPECT_EQ(result.error, FrameDecodeError::InvalidMagic);
    EXPECT_FALSE(result.ok());
}

TEST_F(FrameDecodeResultTest, FrameTooLarge) {
    FrameDecodeLimits small_limit{};
    small_limit.max_payload_bytes = 1024;

    StreamBuffer buf;
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'C'};
    buf.append(magic.data(), 4);
    uint32_t net_len = htonl(2048); // larger than max
    buf.append(reinterpret_cast<const uint8_t*>(&net_len), 4);
    auto result = try_decode_wireframe(buf, small_limit);
    EXPECT_EQ(result.error, FrameDecodeError::FrameTooLarge);
    EXPECT_FALSE(result.ok());
}

TEST_F(FrameDecodeResultTest, LengthMismatchTooShort) {
    auto bytes = make_empty_frame_bytes();

    // Advertise more payload than exists by modifying the length field
    StreamBuffer overlarge(bytes);
    uint32_t inflated = htonl(100); // declare 100 bytes, but buffer has 0
    std::memcpy(overlarge.data() + 4, &inflated, 4);

    auto result = try_decode_wireframe(overlarge, limits);
    EXPECT_EQ(result.error, FrameDecodeError::LengthMismatch);
    EXPECT_FALSE(result.ok());
}

TEST_F(FrameDecodeResultTest, TrailingBytes) {
    auto bytes = make_empty_frame_bytes();
    // Append extra bytes after the valid frame
    bytes.append(std::array<uint8_t, 4>{0xAA, 0xBB, 0xCC, 0xDD}.data(), 4);

    auto result = try_decode_wireframe(bytes, limits);
    EXPECT_EQ(result.error, FrameDecodeError::TrailingBytes);
    EXPECT_FALSE(result.ok());
}

TEST_F(FrameDecodeResultTest, TrailingBytesAllowed) {
    limits.reject_trailing_bytes = false;
    auto bytes = make_empty_frame_bytes();
    bytes.append(std::array<uint8_t, 4>{0xAA, 0xBB, 0xCC, 0xDD}.data(), 4);

    auto result = try_decode_wireframe(bytes, limits);
    EXPECT_EQ(result.error, FrameDecodeError::None);
    EXPECT_TRUE(result.ok());
}

TEST_F(FrameDecodeResultTest, InvalidProtobuf) {
    StreamBuffer buf;
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'C'};
    buf.append(magic.data(), 4);
    const uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t net_len = htonl(sizeof(garbage));
    buf.append(reinterpret_cast<const uint8_t*>(&net_len), 4);
    buf.append(garbage, sizeof(garbage));

    auto result = try_decode_wireframe(buf, limits);
    EXPECT_EQ(result.error, FrameDecodeError::InvalidProtobuf);
    EXPECT_FALSE(result.ok());
}

TEST_F(FrameDecodeResultTest, ValidEnvelopeUnknownPayload) {
    // An empty but valid protobuf WireEnvelope has payload_type() == Unknown
    auto bytes = make_empty_frame_bytes();
    auto result = try_decode_wireframe(bytes, limits);
    EXPECT_EQ(result.error, FrameDecodeError::None);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.frame.payload_type(), WireFrame::PayloadType::Unknown);
}

TEST_F(FrameDecodeResultTest, CompatibilityDecodeReturnsEmptyOnError) {
    StreamBuffer tiny;
    tiny.append(std::array<uint8_t, 3>{'H', 'P', 'A'}.data(), 3);
    auto frame = WireFrame::decode(tiny);
    EXPECT_EQ(frame.payload_type(), WireFrame::PayloadType::Unknown);
}

TEST_F(FrameDecodeResultTest, RoundTripDataFrame) {
    WireFrame f;
    ::hpactor::net::ActorMsgFrame data;
    data.set_type_tag(42);
    data.set_message_id(12345);
    std::string payload = "hello";
    data.set_payload(payload.data(), payload.size());
    *f.pb_envelope.mutable_data_frame() = std::move(data);
    auto encoded = f.encode();

    auto result = try_decode_wireframe(encoded, limits);
    EXPECT_TRUE(result.ok()) << "Decode error: " << static_cast<int>(result.error);
    EXPECT_EQ(result.frame.payload_type(), WireFrame::PayloadType::Data);
}

TEST_F(FrameDecodeResultTest, DeclaredPayloadBytesPreserved) {
    WireFrame f;
    ::hpactor::net::ActorMsgFrame data;
    data.set_type_tag(42);
    *f.pb_envelope.mutable_data_frame() = std::move(data);
    auto encoded = f.encode();

    auto result = try_decode_wireframe(encoded, limits);
    EXPECT_TRUE(result.ok());
    EXPECT_GT(result.declared_payload_bytes, 0u);
}

// ── FrameDispatchResult trivial tests ─────────────────────────────────────

class FrameDispatchResultTest : public ::testing::Test {};

TEST_F(FrameDispatchResultTest, DefaultHasCorrectSize) {
    // Fixed-size diagnostic — no dynamic allocation
    FrameDispatchResult r{};
    EXPECT_EQ(r.code, FrameDispatchCode::ActorDelivered);
    EXPECT_EQ(r.payload_type, WireFrame::PayloadType::Unknown);
}

} // namespace
} // namespace hpactor::net
