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

#include <hpactor/adt/stream_buffer.hpp>

#include <gtest/gtest.h>
#include <vector>

using namespace hpactor;
using namespace hpactor::adt;

// =============================================================================
// StreamBuffer::from_data() tests
// =============================================================================

TEST(StreamBufferFromData, ExactCapacitySmallData) {
    uint8_t data[32];
    for (size_t i = 0; i < 32; ++i)
        data[i] = static_cast<uint8_t>(i);

    auto sb = StreamBuffer::from_data(data, 32);

    EXPECT_EQ(sb.size(), 32u);
    // Capacity must not be the 64KB default minimum — it should be
    // close to the actual data size.
    EXPECT_LE(sb.capacity(), 64u);
    EXPECT_GE(sb.capacity(), 32u);
    for (size_t i = 0; i < 32; ++i)
        EXPECT_EQ(sb[i], data[i]) << "mismatch at index " << i;
}

TEST(StreamBufferFromData, EmptyData) {
    auto sb = StreamBuffer::from_data(nullptr, 0);
    EXPECT_TRUE(sb.empty());
    EXPECT_EQ(sb.size(), 0u);
}

TEST(StreamBufferFromData, LargeDataUsesAdequateCapacity) {
    std::vector<uint8_t> data(100000, 0x42);
    auto sb = StreamBuffer::from_data(data.data(), data.size());
    EXPECT_EQ(sb.size(), 100000u);
    EXPECT_GE(sb.capacity(), 100000u);
    EXPECT_EQ(sb[0], 0x42);
    EXPECT_EQ(sb[99999], 0x42);
}

TEST(StreamBufferFromData, SingleByte) {
    uint8_t data[] = {0xAB};
    auto sb = StreamBuffer::from_data(data, 1);
    EXPECT_EQ(sb.size(), 1u);
    EXPECT_EQ(sb[0], 0xAB);
    EXPECT_LE(sb.capacity(), 64u);
}

TEST(StreamBufferFromData, FromDataIsIndependent) {
    // Mutating the original buffer must not affect the StreamBuffer.
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto sb = StreamBuffer::from_data(data, 8);
    data[0] = 99;
    EXPECT_EQ(sb[0], 1); // StreamBuffer has its own copy
}

TEST(StreamBufferFromData, CapacityDoesNotTriggerDefaultMinimum) {
    // The key invariant: small buffers must not allocate 64KB.
    uint8_t data[16] = {};
    auto sb = StreamBuffer::from_data(data, 16);
    EXPECT_EQ(sb.size(), 16u);
    // The capacity should be close to 16, not 65536.
    EXPECT_LT(sb.capacity(), 1024u); // far below the 64KB default
}
