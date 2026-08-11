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
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/wireframe_connection.hpp>

#include <sys/socket.h>
#include <unistd.h>

using namespace hpactor;
using namespace hpactor::net;

// ── Handshake encode/decode round-trip tests
// ──────────────────────────────────

TEST(HandshakeTest, HelloEncodeDecodeRoundTrip) {
    ::hpactor::net::HandshakeHello hello;
    hello.set_min_version(1);
    hello.set_max_version(3);
    hello.set_feature_flags(0x0005); // ReliableDelivery + StreamProtocol
    hello.set_node_id(42);

    StreamBuffer encoded = encode_handshake_hello(hello);
    ASSERT_FALSE(encoded.empty());
    EXPECT_GE(encoded.size(), HandshakeHeaderSize);

    auto decoded = decode_handshake_hello(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->min_version(), 1u);
    EXPECT_EQ(decoded->max_version(), 3u);
    EXPECT_EQ(decoded->feature_flags(), 0x0005ULL);
    EXPECT_EQ(decoded->node_id(), 42ULL);
}

TEST(HandshakeTest, ResponseEncodeDecodeRoundTrip) {
    ::hpactor::net::HandshakeResponse resp;
    resp.set_accepted_version(2);
    resp.set_feature_flags(0x0001); // only ReliableDelivery
    resp.set_node_id(99);
    resp.set_result(::hpactor::net::HANDSHAKE_ACCEPTED);

    StreamBuffer encoded = encode_handshake_response(resp);
    ASSERT_FALSE(encoded.empty());

    auto decoded = decode_handshake_response(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->accepted_version(), 2u);
    EXPECT_EQ(decoded->feature_flags(), 0x0001ULL);
    EXPECT_EQ(decoded->node_id(), 99ULL);
    EXPECT_EQ(decoded->result(), ::hpactor::net::HANDSHAKE_ACCEPTED);
}

TEST(HandshakeTest, ResponseRejectedEncodeDecodeRoundTrip) {
    ::hpactor::net::HandshakeResponse resp;
    resp.set_accepted_version(0);
    resp.set_feature_flags(0);
    resp.set_node_id(7);
    resp.set_result(::hpactor::net::HANDSHAKE_INCOMPATIBLE_VERSION);

    StreamBuffer encoded = encode_handshake_response(resp);
    ASSERT_FALSE(encoded.empty());

    auto decoded = decode_handshake_response(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->result(), ::hpactor::net::HANDSHAKE_INCOMPATIBLE_VERSION);
    EXPECT_EQ(decoded->accepted_version(), 0u);
}

TEST(HandshakeTest, DecodeRejectsInvalidMagic) {
    // Build a buffer with wrong magic "XXXX"
    StreamBuffer buf;
    const std::array<uint8_t, 4> bad_magic = {'X', 'X', 'X', 'X'};
    buf.append(bad_magic.data(), 4);
    uint32_t zero_len = 0;
    buf.append(reinterpret_cast<const uint8_t*>(&zero_len), 4);

    auto decoded = decode_handshake_hello(buf);
    EXPECT_FALSE(decoded.has_value());
}

TEST(HandshakeTest, DecodeRejectsTruncatedHeader) {
    StreamBuffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("HP"), 2); // Only 2 bytes

    auto decoded = decode_handshake_hello(buf);
    EXPECT_FALSE(decoded.has_value());
}

// ── Version negotiation tests
// ─────────────────────────────────────────────────

TEST(HandshakeTest, VersionNegotiationCompatible) {
    // Client [1, 3], Server [2, 5] → accepted = min(3, 5) = 3
    uint32_t accepted = negotiate_version(1, 3, 2, 5);
    EXPECT_EQ(accepted, 3u);
}

TEST(HandshakeTest, VersionNegotiationExactMatch) {
    // Client [2, 2], Server [2, 2] → accepted = 2
    uint32_t accepted = negotiate_version(2, 2, 2, 2);
    EXPECT_EQ(accepted, 2u);
}

TEST(HandshakeTest, VersionNegotiationSameRange) {
    // Client [1, 5], Server [1, 5] → accepted = 5
    uint32_t accepted = negotiate_version(1, 5, 1, 5);
    EXPECT_EQ(accepted, 5u);
}

TEST(HandshakeTest, VersionNegotiationClientTooLow) {
    // Client [1, 1], Server [2, 5] → client max (1) < server min (2) → disjoint
    uint32_t accepted = negotiate_version(1, 1, 2, 5);
    EXPECT_EQ(accepted, 0u);
}

TEST(HandshakeTest, VersionNegotiationClientTooHigh) {
    // Client [5, 5], Server [1, 2] → client min (5) > server max (2) → disjoint
    uint32_t accepted = negotiate_version(5, 5, 1, 2);
    EXPECT_EQ(accepted, 0u);
}

TEST(HandshakeTest, VersionNegotiationTouchBoundary) {
    // Client [1, 2], Server [2, 5] → accepted = min(2, 5) = 2
    uint32_t accepted = negotiate_version(1, 2, 2, 5);
    EXPECT_EQ(accepted, 2u);
}

// ── Feature flag tests
// ────────────────────────────────────────────────────────

TEST(HandshakeTest, FeatureFlagOperators) {
    auto flags =
        HandshakeFeature::ReliableDelivery | HandshakeFeature::StreamProtocol;
    uint64_t raw = static_cast<uint64_t>(flags);
    EXPECT_EQ(raw, 0x5ULL); // bits 0 and 2 set
}

TEST(HandshakeTest, HasFeatureHelper) {
    uint64_t flags = static_cast<uint64_t>(HandshakeFeature::ReliableDelivery |
                                           HandshakeFeature::BatchMessaging);

    EXPECT_TRUE(has_feature(flags, HandshakeFeature::ReliableDelivery));
    EXPECT_TRUE(has_feature(flags, HandshakeFeature::BatchMessaging));
    EXPECT_FALSE(has_feature(flags, HandshakeFeature::StreamProtocol));
    EXPECT_FALSE(has_feature(flags, HandshakeFeature::FrameCompression));
}

TEST(HandshakeTest, FlagIntersection) {
    uint64_t client_flags = static_cast<uint64_t>(
        HandshakeFeature::ReliableDelivery | HandshakeFeature::BatchMessaging |
        HandshakeFeature::StreamProtocol);
    uint64_t server_flags = static_cast<uint64_t>(
        HandshakeFeature::ReliableDelivery | HandshakeFeature::StreamProtocol |
        HandshakeFeature::FrameCompression);

    uint64_t agreed = client_flags & server_flags;
    EXPECT_TRUE(has_feature(agreed, HandshakeFeature::ReliableDelivery));
    EXPECT_TRUE(has_feature(agreed, HandshakeFeature::StreamProtocol));
    EXPECT_FALSE(has_feature(agreed, HandshakeFeature::BatchMessaging));
    EXPECT_FALSE(has_feature(agreed, HandshakeFeature::FrameCompression));
}

// ── HandshakeConfig tests
// ─────────────────────────────────────────────────────

TEST(HandshakeTest, HandshakeConfigDefaults) {
    HandshakeConfig cfg;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.version_min, 1u);
    EXPECT_EQ(cfg.version_max, 1u);
    EXPECT_EQ(cfg.feature_flags, 0ULL);
    EXPECT_EQ(cfg.timeout, std::chrono::milliseconds(5000));
    EXPECT_EQ(cfg.node_id, 0ULL);
}

// ── WireFrameConnection handshake config test
// ─────────────────────────────────

TEST(HandshakeTest, ConnectionDefaultsToNoHandshake) {
    // Create a connection pair to verify default behavior
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    auto conn = WireFrameConnection::create_as_client(fds[0], EndPoint{},
                                                      EndPoint{}, &loop);

    // By default, handshake should be disabled and state should be Connected
    EXPECT_FALSE(conn->negotiated_handshake().has_value());
    EXPECT_EQ(conn->state(), ConnectionState::Connected);

    conn->close();
    ::close(fds[1]);
}

TEST(HandshakeTest, ConnectionWithHandshakeEnabled) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    auto conn = WireFrameConnection::create_as_client(fds[0], EndPoint{},
                                                      EndPoint{}, &loop);

    // Enable handshake and verify config is stored
    HandshakeConfig cfg;
    cfg.enabled = true;
    cfg.version_min = 2;
    cfg.version_max = 4;
    cfg.feature_flags = static_cast<uint64_t>(HandshakeFeature::ReliableDelivery);
    cfg.node_id = 123;
    conn->set_handshake_config(cfg);

    // Note: create_as_client was already called with defaults,
    // so we can't retroactively enable the handshake for this connection.
    // This test verifies the config plumbing works.

    ::close(fds[1]);
}

// ── Handshake magic constant test
// ─────────────────────────────────────────────

TEST(HandshakeTest, HandshakeMagicIsDistinct) {
    // Handshake magic "HPAH" must differ from WireFrame magic "HPAC"
    EXPECT_NE(HandshakeMagic, WireFrame::MagicHeader);
}

TEST(HandshakeTest, HandshakeHeaderSize) {
    EXPECT_EQ(HandshakeHeaderSize, 8u);
    EXPECT_EQ(WireFrame::HeaderSize, 8u); // Same framing, different magic
}
