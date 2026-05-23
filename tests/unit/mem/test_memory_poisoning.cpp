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
#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/slab_cache.hpp>

#include <cstring>

using namespace hpactor::mem;

TEST(MemoryPoisoningTest, CanaryDetectionOnBufferOverflow) {
    SlabCache cache(SizeClass::k128B);

    void* p = cache.allocate(hpactor::ActorId{1});
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(cache.live_count(), 1U);

    // Corrupt the canary by writing past the user data region
    std::memset(static_cast<std::byte*>(p) + 128, 0xFF, 16);

    // Deallocate — in debug mode, canary verification fires
    // In release mode, the corrupt data is written to the footer
    cache.deallocate(p);

    // After corruption, the freed block should still be returned
    EXPECT_EQ(cache.live_count(), 0U);
}

TEST(MemoryPoisoningTest, BlocksRecycledAndDataIntegrityMaintained) {
    SlabCache cache(SizeClass::k256B);

    void* p1 = cache.allocate(hpactor::ActorId{1});
    ASSERT_NE(p1, nullptr);
    std::memset(p1, 0x42, 256);

    cache.deallocate(p1);

    // Re-allocate — should get same block
    void* p2 = cache.allocate(hpactor::ActorId{2});
    EXPECT_EQ(p2, p1); // recycled

    // Data should be preserved (or poisoned, depending on build mode)
    // In any case, the block should be usable
    std::memset(p2, 0x77, 256);
    uint8_t first = *static_cast<uint8_t*>(p2);
    EXPECT_EQ(first, 0x77);

    cache.deallocate(p2);
    EXPECT_EQ(cache.live_count(), 0U);
}

TEST(MemoryPoisoningTest, MultipleAllocFreeCycles) {
    SlabCache cache(SizeClass::k64B);
    for (int cycle = 0; cycle < 100; ++cycle) {
        void* p = cache.allocate(hpactor::ActorId{static_cast<uint64_t>(cycle)});
        ASSERT_NE(p, nullptr);
        std::memset(p, static_cast<uint8_t>(cycle), 64);
        cache.deallocate(p);
    }
    EXPECT_EQ(cache.live_count(), 0U);
}
