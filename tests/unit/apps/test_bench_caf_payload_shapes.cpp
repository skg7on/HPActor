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

#include <apps/bench_caf/caf_bench_config.hpp>
#include <apps/bench_caf/messages.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(PayloadShapes, ProtobufSmallRoundTrips) {
    bench_caf::BenchPayloadHeader header{7, 42, 100};
    auto payload = bench_caf::encode_shaped_payload(
        header, 64, bench_caf::MessageShape::ProtobufSmall, 1);
    EXPECT_GE(payload.size(), 64u);
    auto decoded = bench_caf::decode_bench_payload(payload);
    EXPECT_EQ(decoded.sender_id, 7u);
    EXPECT_EQ(decoded.sequence, 42u);
    EXPECT_EQ(decoded.timestamp_us, 100u);
}

TEST(PayloadShapes, ProtobufNestedRespectsSize) {
    bench_caf::BenchPayloadHeader header{1, 1, 1};
    auto payload = bench_caf::encode_shaped_payload(
        header, 256, bench_caf::MessageShape::ProtobufNested, 2);
    EXPECT_GE(payload.size(), 256u);
}

TEST(PayloadShapes, SharedBufferRoundTrips) {
    bench_caf::BenchPayloadHeader header{9, 8, 7};
    auto payload = bench_caf::encode_shaped_payload(
        header, 128, bench_caf::MessageShape::SharedBuffer, 3);
    EXPECT_GE(payload.size(), bench_caf::BenchPayloadHeader::kEncodedSize);
    auto decoded = bench_caf::decode_bench_payload(payload);
    EXPECT_EQ(decoded.sender_id, 9u);
}

TEST(PayloadShapes, Mixed80_20UsesCorrectSizes) {
    size_t small = 0, large = 0;
    bench_caf::BenchPayloadHeader header{};
    for (int i = 0; i < 100; ++i) {
        auto payload = bench_caf::encode_shaped_payload(
            header, 1024, bench_caf::MessageShape::Mixed80_20,
            static_cast<uint64_t>(i));
        if (payload.size() <= 64)
            ++small;
        else
            ++large;
    }
    EXPECT_GE(small, 60u);
    EXPECT_GE(large, 10u);
}

TEST(PayloadShapes, HeaderOnlyWorksWithShapedFactory) {
    bench_caf::BenchPayloadHeader header{1, 2, 3};
    auto payload = bench_caf::encode_shaped_payload(
        header, 0, bench_caf::MessageShape::HeaderOnly, 42);
    EXPECT_EQ(payload.size(), bench_caf::BenchPayloadHeader::kEncodedSize);
    auto decoded = bench_caf::decode_bench_payload(payload);
    EXPECT_EQ(decoded.sender_id, 1u);
}

TEST(PayloadShapes, FixedBytesWorksWithShapedFactory) {
    bench_caf::BenchPayloadHeader header{5, 6, 7};
    auto payload = bench_caf::encode_shaped_payload(
        header, 512, bench_caf::MessageShape::FixedBytes, 99);
    EXPECT_EQ(payload.size(), 512u);
}
