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
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/slab_cache.hpp>

#include <cstring>
#include <vector>

using namespace hpactor::mem;

TEST(SlabCacheTest, InitialState) {
    SlabCache cache(SizeClass::k128B);
    EXPECT_EQ(cache.size_class(), SizeClass::k128B);
    EXPECT_EQ(cache.live_count(), 0U);
}

TEST(SlabCacheTest, AllocateAndFree) {
    SlabCache cache(SizeClass::k128B);

    void* b1 = cache.allocate(hpactor::ActorId{1});
    void* b2 = cache.allocate(hpactor::ActorId{2});
    void* b3 = cache.allocate(hpactor::ActorId{3});

    ASSERT_NE(b1, nullptr);
    ASSERT_NE(b2, nullptr);
    ASSERT_NE(b3, nullptr);
    EXPECT_NE(b1, b2);
    EXPECT_NE(b2, b3);
    EXPECT_EQ(cache.live_count(), 3U);

    // Each block should be independently writeable
    std::memset(b1, 0x11, 128);
    std::memset(b2, 0x22, 128);
    std::memset(b3, 0x33, 128);
    EXPECT_EQ(*static_cast<uint8_t*>(b1), 0x11);
    EXPECT_EQ(*static_cast<uint8_t*>(b2), 0x22);
    EXPECT_EQ(*static_cast<uint8_t*>(b3), 0x33);

    cache.deallocate(b1);
    cache.deallocate(b2);
    cache.deallocate(b3);
    EXPECT_EQ(cache.live_count(), 0U);
}

TEST(SlabCacheTest, RecycleFromFreelist) {
    SlabCache cache(SizeClass::k128B);

    void* b1 = cache.allocate(hpactor::ActorId{1});
    cache.deallocate(b1);
    EXPECT_EQ(cache.live_count(), 0U);

    void* b2 = cache.allocate(hpactor::ActorId{2});
    ASSERT_NE(b2, nullptr);
    EXPECT_EQ(b2, b1); // recycled from freelist

    cache.deallocate(b2);
}

TEST(SlabCacheTest, MultipleSlabs) {
    SlabCache cache(SizeClass::k128B);
    std::vector<void*> blocks;

    for (int i = 0; i < 1000; ++i) {
        void* b = cache.allocate(hpactor::ActorId{static_cast<uint64_t>(i)});
        ASSERT_NE(b, nullptr);
        std::memset(b, static_cast<uint8_t>(i & 0xFF), 128);
        blocks.push_back(b);
    }
    EXPECT_EQ(cache.live_count(), 1000U);

    for (auto* b : blocks) {
        cache.deallocate(b);
    }
    EXPECT_EQ(cache.live_count(), 0U);
}

TEST(SlabCacheTest, Stats) {
    SlabCache cache(SizeClass::k128B);

    void* b1 = cache.allocate(hpactor::ActorId{1});
    void* b2 = cache.allocate(hpactor::ActorId{2});
    void* b3 = cache.allocate(hpactor::ActorId{3});
    cache.deallocate(b1);

    void* b4 = cache.allocate(hpactor::ActorId{4}); // recycled

    // Allocate many to force multiple slabs
    std::vector<void*> blocks;
    for (int i = 0; i < 1000; ++i) {
        blocks.push_back(
            cache.allocate(hpactor::ActorId{static_cast<uint64_t>(1000 + i)}));
    }
    for (auto* b : blocks) {
        cache.deallocate(b);
    }
    cache.deallocate(b2);
    cache.deallocate(b3);
    cache.deallocate(b4);

    auto& stats = cache.stats();
    EXPECT_EQ(stats.alloc_count.load(), 1004U); // 3 + 1 (b4) + 1000
    EXPECT_EQ(stats.free_count.load(), 1004U);
}
