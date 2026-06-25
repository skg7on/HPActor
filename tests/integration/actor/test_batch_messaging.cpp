// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <hpactor/msg/frame.hpp>
#include <hpactor/types/types.hpp>

using namespace hpactor;
using namespace hpactor::net;

// ── Batch frame encode/decode roundtrip tests ───────────────────────────

TEST(BatchMessagingTest, BatchFrameEncodeDecodeRoundtrip) {
    ActorAddress sender(endpoint_ops::parse_endpoint("node1:12345"), 10,
                        ActorId(1), 5);
    ActorAddress receiver(endpoint_ops::parse_endpoint("node2:12345"), 20,
                          ActorId(2), 6);

    BatchMsgFrame batch;
    to_proto(batch.mutable_sender(), sender);
    to_proto(batch.mutable_receiver(), receiver);

    for (int i = 0; i < 3; ++i) {
        auto* entry = batch.add_entries();
        entry->set_type_tag(static_cast<uint32_t>(TypeTag::User));
        entry->set_message_id(static_cast<uint64_t>(i) + 1);
        std::string payload = "msg" + std::to_string(i);
        entry->set_payload(payload.data(), payload.size());
        if (i == 0) {
            entry->set_flags(WireFrame::AckRequested);
        }
    }

    auto frame = WireFrame::from_batch(batch);
    EXPECT_EQ(frame.payload_type(), WireFrame::PayloadType::Batch);

    auto encoded = frame.encode();
    auto decoded = WireFrame::decode(encoded);

    EXPECT_EQ(decoded.payload_type(), WireFrame::PayloadType::Batch);
    ASSERT_TRUE(decoded.pb_envelope.has_batch_frame());
    const auto& decoded_batch = decoded.pb_envelope.batch_frame();
    EXPECT_EQ(decoded_batch.entries_size(), 3);
    EXPECT_EQ(decoded_batch.entries(0).message_id(), 1u);
    EXPECT_EQ(decoded_batch.entries(2).payload(), "msg2");
    EXPECT_EQ(decoded_batch.entries(0).flags() & WireFrame::AckRequested,
              WireFrame::AckRequested);
}

TEST(BatchMessagingTest, BatchFrameEmptyBatch) {
    BatchMsgFrame batch;
    auto frame = WireFrame::from_batch(batch);
    EXPECT_EQ(frame.payload_type(), WireFrame::PayloadType::Batch);

    auto encoded = frame.encode();
    auto decoded = WireFrame::decode(encoded);
    EXPECT_EQ(decoded.payload_type(), WireFrame::PayloadType::Batch);
    EXPECT_EQ(decoded.pb_envelope.batch_frame().entries_size(), 0);
}

TEST(BatchMessagingTest, BatchFrameSingleEntry) {
    BatchMsgFrame batch;
    auto* entry = batch.add_entries();
    entry->set_type_tag(static_cast<uint32_t>(TypeTag::User));
    entry->set_payload("hello", 5u);
    entry->set_flags(WireFrame::Important | WireFrame::AckRequested);

    auto frame = WireFrame::from_batch(batch);
    auto encoded = frame.encode();
    auto decoded = WireFrame::decode(encoded);

    ASSERT_EQ(decoded.pb_envelope.batch_frame().entries_size(), 1);
    const auto& e = decoded.pb_envelope.batch_frame().entries(0);
    EXPECT_EQ(e.type_tag(), static_cast<uint32_t>(TypeTag::User));
    EXPECT_EQ(e.payload(), "hello");
    // Both flags survive roundtrip
    EXPECT_NE(e.flags() & WireFrame::Important, 0u);
    EXPECT_NE(e.flags() & WireFrame::AckRequested, 0u);
}

TEST(BatchMessagingTest, WireFramePayloadTypeDiscriminates) {
    // Data frame
    ActorMsgFrame data_msg;
    data_msg.set_type_tag(42);
    auto data_frame = WireFrame::from_data(std::move(data_msg));
    EXPECT_EQ(data_frame.payload_type(), WireFrame::PayloadType::Data);

    // Batch frame
    BatchMsgFrame batch_msg;
    auto batch_frame = WireFrame::from_batch(std::move(batch_msg));
    EXPECT_EQ(batch_frame.payload_type(), WireFrame::PayloadType::Batch);

    // Default (unknown)
    WireFrame unknown_frame;
    EXPECT_EQ(unknown_frame.payload_type(), WireFrame::PayloadType::Unknown);
}

TEST(BatchMessagingTest, WireFrameFromDataEncodesDecodes) {
    ActorAddress sender(endpoint_ops::parse_endpoint("node1:12345"), 10,
                        ActorId(1), 5);
    ActorAddress receiver(endpoint_ops::parse_endpoint("node2:12345"), 20,
                          ActorId(2), 6);

    ActorMsgFrame data_msg;
    to_proto(data_msg.mutable_sender(), sender);
    to_proto(data_msg.mutable_receiver(), receiver);
    data_msg.set_type_tag(static_cast<uint32_t>(TypeTag::User));
    data_msg.set_message_id(42u);
    data_msg.set_payload("test-data", 9u);

    auto frame = WireFrame::from_data(std::move(data_msg));
    EXPECT_EQ(frame.payload_type(), WireFrame::PayloadType::Data);

    auto encoded = frame.encode();
    auto decoded = WireFrame::decode(encoded);

    EXPECT_EQ(decoded.payload_type(), WireFrame::PayloadType::Data);
    ASSERT_TRUE(decoded.pb_envelope.has_data_frame());
    EXPECT_EQ(decoded.pb_envelope.data_frame().payload(), "test-data");
    EXPECT_EQ(decoded.pb_envelope.data_frame().message_id(), 42u);
}

TEST(BatchMessagingTest, DecodeInvalidMagicReturnsUnknown) {
    StreamBuffer bad;
    bad.reserve(8);
    const std::array<uint8_t, 4> bad_magic = {'B', 'A', 'D', '!'};
    bad.append(bad_magic.data(), 4);
    uint32_t zero = 0;
    bad.append(reinterpret_cast<const uint8_t*>(&zero), 4);

    auto decoded = WireFrame::decode(bad);
    EXPECT_EQ(decoded.payload_type(), WireFrame::PayloadType::Unknown);
}

TEST(BatchMessagingTest, DecodeTruncatedReturnsUnknown) {
    BatchMsgFrame batch;
    auto* entry = batch.add_entries();
    entry->set_type_tag(static_cast<uint32_t>(TypeTag::User));
    entry->set_payload("hello", 5);
    auto frame = WireFrame::from_batch(batch);
    auto full = frame.encode();

    // Truncate: copy all but last byte
    StreamBuffer truncated(full.begin(), full.end() - 1);
    auto decoded = WireFrame::decode(truncated);
    EXPECT_EQ(decoded.payload_type(), WireFrame::PayloadType::Unknown);
}

TEST(BatchMessagingTest, BatchWithTraceContextRoundtrip) {
    BatchMsgFrame batch;
    auto* entry = batch.add_entries();
    entry->set_type_tag(static_cast<uint32_t>(TypeTag::User));
    entry->set_payload("data", 4u);

    // Fill trace context bytes directly on the proto entry
    auto* pb_tc = entry->mutable_trace_context();
    std::array<uint8_t, 16> tid{1, 2,  3,  4,  5,  6,  7,  8,
                                9, 10, 11, 12, 13, 14, 15, 16};
    std::array<uint8_t, 8> sid{100, 101, 102, 103, 104, 105, 106, 107};
    pb_tc->set_trace_id(tid.data(), tid.size());
    pb_tc->set_span_id(sid.data(), sid.size());
    pb_tc->set_flags(1u); // sampled flag

    auto frame = WireFrame::from_batch(batch);
    auto encoded = frame.encode();
    auto decoded = WireFrame::decode(encoded);

    ASSERT_EQ(decoded.pb_envelope.batch_frame().entries_size(), 1);
    const auto& dec_entry = decoded.pb_envelope.batch_frame().entries(0);
    EXPECT_TRUE(dec_entry.has_trace_context());
    EXPECT_EQ(dec_entry.trace_context().flags(), 1u);
}
