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

#include <apps/bench_caf/messages.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafPayloads, HeaderOnlyRoundTrips) {
    bench_caf::BenchPayloadHeader header;
    header.sender_id = 3;
    header.sequence = 9;
    header.timestamp_us = 17;

    auto payload = bench_caf::encode_bench_payload(header, 0, 123);
    auto decoded = bench_caf::decode_bench_payload(payload);

    EXPECT_EQ(payload.size(), bench_caf::BenchPayloadHeader::kEncodedSize);
    EXPECT_EQ(decoded.sender_id, 3u);
    EXPECT_EQ(decoded.sequence, 9u);
    EXPECT_EQ(decoded.timestamp_us, 17u);
}

TEST(BenchCafPayloads, FixedBytesRespectsMinimumHeaderSize) {
    bench_caf::BenchPayloadHeader header;
    auto payload = bench_caf::encode_bench_payload(header, 4, 123);
    EXPECT_EQ(payload.size(), bench_caf::BenchPayloadHeader::kEncodedSize);
}

TEST(BenchCafPayloads, FixedBytesUsesRequestedSize) {
    bench_caf::BenchPayloadHeader header;
    auto payload = bench_caf::encode_bench_payload(header, 1024, 123);
    EXPECT_EQ(payload.size(), 1024u);
}
