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

using namespace hpactor;

TEST(StreamFrameTest, StreamOpenFrameRoundtrip) {
    net::StreamOpenFrame open;
    open.set_stream_id(42);
    open.set_initial_window_bytes(65536);

    auto frame = net::WireFrame::from_stream_open(std::move(open));
    EXPECT_EQ(frame.payload_type(), net::WireFrame::PayloadType::StreamOpen);

    auto encoded = frame.encode();
    EXPECT_FALSE(encoded.empty());

    auto decoded = net::WireFrame::decode(encoded);
    ASSERT_EQ(decoded.payload_type(), net::WireFrame::PayloadType::StreamOpen);
    EXPECT_EQ(decoded.pb_envelope.stream_open().stream_id(), 42u);
    EXPECT_EQ(decoded.pb_envelope.stream_open().initial_window_bytes(), 65536u);
}

TEST(StreamFrameTest, StreamDataFrameRoundtrip) {
    net::StreamDataFrame data;
    data.set_stream_id(42);
    data.set_sequence(7);
    data.set_payload("hello", 5);

    auto frame = net::WireFrame::from_stream_data(std::move(data));
    EXPECT_EQ(frame.payload_type(), net::WireFrame::PayloadType::StreamData);

    auto encoded = frame.encode();
    EXPECT_FALSE(encoded.empty());

    auto decoded = net::WireFrame::decode(encoded);
    ASSERT_EQ(decoded.payload_type(), net::WireFrame::PayloadType::StreamData);
    EXPECT_EQ(decoded.pb_envelope.stream_data().stream_id(), 42u);
    EXPECT_EQ(decoded.pb_envelope.stream_data().sequence(), 7u);
    EXPECT_EQ(decoded.pb_envelope.stream_data().payload(), "hello");
}

TEST(StreamFrameTest, StreamAckFrameRoundtrip) {
    net::StreamAckFrame ack;
    ack.set_stream_id(42);
    ack.set_last_sequence(10);
    ack.set_window_bytes(32768);

    auto frame = net::WireFrame::from_stream_ack(std::move(ack));
    EXPECT_EQ(frame.payload_type(), net::WireFrame::PayloadType::StreamAck);

    auto encoded = frame.encode();
    EXPECT_FALSE(encoded.empty());

    auto decoded = net::WireFrame::decode(encoded);
    ASSERT_EQ(decoded.payload_type(), net::WireFrame::PayloadType::StreamAck);
    EXPECT_EQ(decoded.pb_envelope.stream_ack().last_sequence(), 10u);
    EXPECT_EQ(decoded.pb_envelope.stream_ack().window_bytes(), 32768u);
}

TEST(StreamFrameTest, StreamCloseFrameRoundtrip) {
    net::StreamCloseFrame close;
    close.set_stream_id(42);
    close.set_reason(net::StreamCloseFrame::COMPLETE);

    auto frame = net::WireFrame::from_stream_close(std::move(close));
    EXPECT_EQ(frame.payload_type(), net::WireFrame::PayloadType::StreamClose);

    auto encoded = frame.encode();
    EXPECT_FALSE(encoded.empty());

    auto decoded = net::WireFrame::decode(encoded);
    ASSERT_EQ(decoded.payload_type(), net::WireFrame::PayloadType::StreamClose);
    EXPECT_EQ(decoded.pb_envelope.stream_close().reason(),
              net::StreamCloseFrame::COMPLETE);
}

TEST(StreamFrameTest, StreamErrorFrameRoundtrip) {
    net::StreamErrorFrame error;
    error.set_stream_id(42);
    error.set_error_code(3);
    error.set_description("test error");

    auto frame = net::WireFrame::from_stream_error(std::move(error));
    EXPECT_EQ(frame.payload_type(), net::WireFrame::PayloadType::StreamError);

    auto encoded = frame.encode();
    EXPECT_FALSE(encoded.empty());

    auto decoded = net::WireFrame::decode(encoded);
    ASSERT_EQ(decoded.payload_type(), net::WireFrame::PayloadType::StreamError);
    EXPECT_EQ(decoded.pb_envelope.stream_error().error_code(), 3u);
    EXPECT_EQ(decoded.pb_envelope.stream_error().description(), "test error");
}

TEST(StreamFrameTest, PayloadTypeDiscrimination) {
    auto open_frame = net::WireFrame::from_stream_open({});
    EXPECT_EQ(open_frame.payload_type(), net::WireFrame::PayloadType::StreamOpen);

    auto data_frame = net::WireFrame::from_stream_data({});
    EXPECT_EQ(data_frame.payload_type(), net::WireFrame::PayloadType::StreamData);

    auto ack_frame = net::WireFrame::from_stream_ack({});
    EXPECT_EQ(ack_frame.payload_type(), net::WireFrame::PayloadType::StreamAck);

    auto close_frame = net::WireFrame::from_stream_close({});
    EXPECT_EQ(close_frame.payload_type(), net::WireFrame::PayloadType::StreamClose);

    auto error_frame = net::WireFrame::from_stream_error({});
    EXPECT_EQ(error_frame.payload_type(), net::WireFrame::PayloadType::StreamError);
}

TEST(StreamFrameTest, StreamIdUniquenessFormula) {
    // IDs from the same sender are unique (monotonic counter in lower 32 bits).
    uint64_t id1 = (static_cast<uint64_t>(42) << 32) | 1;
    uint64_t id2 = (static_cast<uint64_t>(42) << 32) | 2;
    EXPECT_NE(id1, id2);

    // Different senders produce different IDs even with the same counter.
    uint64_t id3 = (static_cast<uint64_t>(99) << 32) | 1;
    EXPECT_NE(id1, id3);
}

TEST(StreamFrameTest, EncodeDecodeEmptyDescription) {
    net::StreamErrorFrame error;
    error.set_stream_id(1);
    error.set_error_code(5);
    // No description set — should round-trip correctly.

    auto frame = net::WireFrame::from_stream_error(std::move(error));
    auto encoded = frame.encode();
    auto decoded = net::WireFrame::decode(encoded);

    ASSERT_EQ(decoded.payload_type(), net::WireFrame::PayloadType::StreamError);
    EXPECT_EQ(decoded.pb_envelope.stream_error().error_code(), 5u);
    EXPECT_TRUE(decoded.pb_envelope.stream_error().description().empty());
}

TEST(StreamFrameTest, EncodeDecodeZeroPayload) {
    net::StreamDataFrame data;
    data.set_stream_id(1);
    data.set_sequence(1);
    // Empty payload — edge case.

    auto frame = net::WireFrame::from_stream_data(std::move(data));
    auto encoded = frame.encode();
    auto decoded = net::WireFrame::decode(encoded);

    ASSERT_EQ(decoded.payload_type(), net::WireFrame::PayloadType::StreamData);
    EXPECT_EQ(decoded.pb_envelope.stream_data().payload().size(), 0u);
}

TEST(StreamFrameTest, DecodeCorruptFrameReturnsDefault) {
    // Corrupt data does not match the magic header.
    uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
    StreamBuffer bad_data(garbage, garbage + sizeof(garbage));
    auto decoded = net::WireFrame::decode(bad_data);
    EXPECT_EQ(decoded.length, 0u);
}

TEST(StreamFrameTest, DecodeTruncatedFrameReturnsDefault) {
    // Data shorter than the 8-byte header.
    uint8_t short_data[] = {0x48, 0x50, 0x41, 0x43}; // "HPAC" magic, but no
                                                     // length
    StreamBuffer buf(short_data, short_data + 4);
    auto decoded = net::WireFrame::decode(buf);
    EXPECT_EQ(decoded.length, 0u);
}
