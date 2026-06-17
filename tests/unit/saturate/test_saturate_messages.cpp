// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "messages.hpp"

namespace hpactor::apps::bench_saturate {
namespace {

// =============================================================================
// SaturateStartPayload roundtrip
// =============================================================================

TEST(SaturateMessagesTest, StartPayloadRoundtrip) {
    SaturateStartPayload orig;
    orig.num_senders = 500;
    orig.num_receivers = 50;
    orig.payload_mode = PayloadMode::Junk;
    orig.payload_size_min = 1024;
    orig.payload_size_max = 16384;
    orig.initial_rate_msgps = 1000;
    orig.step_interval_ms = 500;
    orig.drop_threshold_pct = 2.5f;
    orig.refine_iterations = 7;
    orig.mailbox_capacity = 2048;
    orig.stable_duration_ms = 10000;
    orig.duration_max_ms = 60000;

    auto buf = orig.encode();
    auto decoded = SaturateStartPayload::decode(buf);

    EXPECT_EQ(decoded.num_senders, 500);
    EXPECT_EQ(decoded.num_receivers, 50);
    EXPECT_EQ(decoded.payload_mode, PayloadMode::Junk);
    EXPECT_EQ(decoded.payload_size_min, 1024);
    EXPECT_EQ(decoded.payload_size_max, 16384);
    EXPECT_EQ(decoded.initial_rate_msgps, 1000);
    EXPECT_EQ(decoded.step_interval_ms, 500);
    EXPECT_FLOAT_EQ(decoded.drop_threshold_pct, 2.5f);
    EXPECT_EQ(decoded.refine_iterations, 7);
    EXPECT_EQ(decoded.mailbox_capacity, 2048);
    EXPECT_EQ(decoded.stable_duration_ms, 10000);
    EXPECT_EQ(decoded.duration_max_ms, 60000);
}

TEST(SaturateMessagesTest, StartPayloadDecodeTruncated) {
    StreamBuffer small_buf(10);
    auto decoded = SaturateStartPayload::decode(small_buf);
    EXPECT_EQ(decoded.num_senders, 100); // default struct value
}

// =============================================================================
// RateChangePayload roundtrip
// =============================================================================

TEST(SaturateMessagesTest, RateChangePayloadRoundtrip) {
    RateChangePayload orig;
    orig.target_rate_msgps = 5000;
    orig.payload_mode = PayloadMode::Mixed;
    orig.payload_size_min = 32;
    orig.payload_size_max = 4096;
    orig.step_interval_ms = 2000;

    auto buf = orig.encode();
    auto decoded = RateChangePayload::decode(buf);

    EXPECT_EQ(decoded.target_rate_msgps, 5000);
    EXPECT_EQ(decoded.payload_mode, PayloadMode::Mixed);
    EXPECT_EQ(decoded.payload_size_min, 32);
    EXPECT_EQ(decoded.payload_size_max, 4096);
    EXPECT_EQ(decoded.step_interval_ms, 2000);
}

TEST(SaturateMessagesTest, RateChangePayloadDecodeTruncated) {
    StreamBuffer small_buf(5);
    auto decoded = RateChangePayload::decode(small_buf);
    EXPECT_EQ(decoded.target_rate_msgps, 100); // default struct value
}

// =============================================================================
// LoadMessagePayload
// =============================================================================

TEST(SaturateMessagesTest, LoadMessageHeaderRoundtrip) {
    LoadMessagePayload orig;
    orig.sender_id = 42;
    orig.seq_no = 123456789012345ULL;
    orig.send_timestamp_us = 9876543210ULL;

    auto buf = orig.encode_header();
    EXPECT_EQ(buf.size(), LoadMessagePayload::kHeaderSize);
    EXPECT_EQ(buf.size(), 20u);

    auto decoded = LoadMessagePayload::decode(buf);
    EXPECT_EQ(decoded.sender_id, 42);
    EXPECT_EQ(decoded.seq_no, 123456789012345ULL);
    EXPECT_EQ(decoded.send_timestamp_us, 9876543210ULL);
}

TEST(SaturateMessagesTest, LoadMessageJunkSize) {
    LoadMessagePayload orig;
    orig.sender_id = 1;
    orig.seq_no = 100;
    orig.send_timestamp_us = 1000;

    size_t total_size = LoadMessagePayload::kHeaderSize + 512;
    auto buf = orig.encode_with_junk(total_size, 42 /* seed */);
    EXPECT_EQ(buf.size(), total_size);
    // Header should still decode correctly
    auto decoded = LoadMessagePayload::decode(buf);
    EXPECT_EQ(decoded.sender_id, 1);
    EXPECT_EQ(decoded.seq_no, 100);
    EXPECT_EQ(decoded.send_timestamp_us, 1000);
}

TEST(SaturateMessagesTest, LoadMessageJunkDeterministic) {
    LoadMessagePayload orig;
    orig.sender_id = 1;
    orig.seq_no = 1;
    orig.send_timestamp_us = 1;

    auto buf1 = orig.encode_with_junk(100, 42);
    auto buf2 = orig.encode_with_junk(100, 42);
    EXPECT_EQ(buf1.size(), buf2.size());
    // Same seed should produce same junk bytes
    EXPECT_EQ(std::memcmp(buf1.data(), buf2.data(), buf1.size()), 0);
}

// =============================================================================
// LatencySamplePayload roundtrip
// =============================================================================

TEST(SaturateMessagesTest, LatencySamplePayloadRoundtrip) {
    LatencySamplePayload orig;
    orig.sender_id = 7;
    orig.seq_no = 999;
    orig.latency_us = 1500;

    auto buf = orig.encode();
    auto decoded = LatencySamplePayload::decode(buf);

    EXPECT_EQ(decoded.sender_id, 7);
    EXPECT_EQ(decoded.seq_no, 999);
    EXPECT_EQ(decoded.latency_us, 1500);
}

// =============================================================================
// DropReportPayload roundtrip
// =============================================================================

TEST(SaturateMessagesTest, DropReportPayloadRoundtrip) {
    DropReportPayload orig;
    orig.receiver_id = 3;
    orig.total_received = 1000000;
    orig.total_dropped = 500;

    auto buf = orig.encode();
    auto decoded = DropReportPayload::decode(buf);

    EXPECT_EQ(decoded.receiver_id, 3);
    EXPECT_EQ(decoded.total_received, 1000000);
    EXPECT_EQ(decoded.total_dropped, 500);
}

// =============================================================================
// PayloadMode enum values
// =============================================================================

TEST(SaturateMessagesTest, PayloadModeValues) {
    EXPECT_EQ(static_cast<uint8_t>(PayloadMode::Small), 0);
    EXPECT_EQ(static_cast<uint8_t>(PayloadMode::Junk), 1);
    EXPECT_EQ(static_cast<uint8_t>(PayloadMode::Mixed), 2);
}

// =============================================================================
// Random payload size
// =============================================================================

TEST(SaturateMessagesTest, RandomPayloadSizeInRange) {
    uint64_t seed = 12345;
    for (int i = 0; i < 1000; ++i) {
        size_t sz = random_payload_size(1024, 4096, seed);
        EXPECT_GE(sz, 1024u);
        EXPECT_LE(sz, 4096u);
    }
}

TEST(SaturateMessagesTest, RandomPayloadSizeDeterministic) {
    uint64_t seed1 = 42, seed2 = 42;
    EXPECT_EQ(random_payload_size(100, 200, seed1),
              random_payload_size(100, 200, seed2));
}

} // namespace
} // namespace hpactor::apps::bench_saturate
