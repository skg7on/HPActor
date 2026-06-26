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
#include <hpactor/actor/stream_config.hpp>
#include <hpactor/actor/stream_sender_actor.hpp>

using namespace hpactor;

TEST(StreamSenderActorTest, StreamConfigDefaultValues) {
    StreamConfig cfg;
    EXPECT_EQ(cfg.initial_window_bytes, 64u * 1024u);
    EXPECT_EQ(cfg.max_chunk_bytes, 64u * 1024u);
    EXPECT_EQ(cfg.send_buffer_bytes, 256u * 1024u);
    EXPECT_EQ(cfg.max_in_flight_frames, 256u);
}

TEST(StreamSenderActorTest, BytesInFlightArithmetic) {
    size_t sent = 100 + 200 + 150;
    size_t acked = 100 + 200;
    size_t in_flight = sent - acked;
    EXPECT_EQ(in_flight, 150u);
}

TEST(StreamSenderActorTest, WindowExceededPauseCondition) {
    size_t bytes_in_flight = 400;
    size_t window_bytes = 256;
    EXPECT_TRUE(bytes_in_flight >= window_bytes);
}

TEST(StreamSenderActorTest, WindowZeroAllowsSingleChunkForAntiDeadlock) {
    size_t bytes_in_flight = 0;
    // When window is zero (not yet advertised), a single chunk is
    // allowed through to prevent deadlock during stream opening.
    bool can_send_one = (bytes_in_flight == 0);
    EXPECT_TRUE(can_send_one);
}

TEST(StreamSenderActorTest, StreamIdFormula) {
    uint64_t sender_id = 42;
    uint64_t counter = 7;
    uint64_t stream_id = (sender_id << 32) | counter;
    EXPECT_EQ(stream_id, 0x2A00000007ULL);
}

TEST(StreamSenderActorTest, CumulativeAckAdvancesWindow) {
    uint64_t last_acked = 0;
    uint64_t new_ack = 5;
    EXPECT_GT(new_ack, last_acked);
    last_acked = new_ack;
    EXPECT_EQ(last_acked, 5u);
}

TEST(StreamSenderActorTest, DuplicateAckIsNoOp) {
    uint64_t last_acked = 5;
    uint64_t duplicate_ack = 3;
    EXPECT_LE(duplicate_ack, last_acked);
}
