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
#include <hpactor/mem/segment_provider.hpp>
#include <hpactor/mem/size_class.hpp>

#include <cstring>
#include <set>

using namespace hpactor::mem;

TEST(SegmentProviderTest, AcquireSlabsAreDistinctAndWritable) {
    SegmentProvider& sp = SegmentProvider::instance();

    void* slab32 = sp.acquire_slab(SizeClass::k32B);
    void* slab64 = sp.acquire_slab(SizeClass::k64B);
    void* slab128 = sp.acquire_slab(SizeClass::k128B);
    void* slab4k = sp.acquire_slab(SizeClass::k4KB);

    ASSERT_NE(slab32, nullptr);
    ASSERT_NE(slab64, nullptr);
    ASSERT_NE(slab128, nullptr);
    ASSERT_NE(slab4k, nullptr);

    // All pointers should be distinct
    std::set<void*> ptrs = {slab32, slab64, slab128, slab4k};
    EXPECT_EQ(ptrs.size(), 4U);

    // Each slab should be writable
    size_t sz32 = sp.slab_size(SizeClass::k32B);
    size_t sz4k = sp.slab_size(SizeClass::k4KB);
    EXPECT_GT(sz32, 0U);
    EXPECT_GT(sz4k, 0U);
    EXPECT_LT(sz32, sz4k);

    std::memset(slab32, 0xAB, sz32);
    std::memset(slab4k, 0xCD, sz4k);

    sp.release_slab(slab32, SizeClass::k32B);
    sp.release_slab(slab64, SizeClass::k64B);
    sp.release_slab(slab128, SizeClass::k128B);
    sp.release_slab(slab4k, SizeClass::k4KB);
}

TEST(SegmentProviderTest, SlabSizeRatios) {
    SegmentProvider& sp = SegmentProvider::instance();

    size_t sz32 = sp.slab_size(SizeClass::k32B);
    EXPECT_EQ(sp.slab_size(SizeClass::k256B), sz32 * 2); // 128KB
    EXPECT_EQ(sp.slab_size(SizeClass::k512B), sz32 * 4); // 256KB
    EXPECT_EQ(sp.slab_size(SizeClass::k4KB), sz32 * 8);  // 512KB
}

TEST(SegmentProviderTest, AddressToSegmentLookup) {
    SegmentProvider& sp = SegmentProvider::instance();

    void* slab32 = sp.acquire_slab(SizeClass::k32B);
    ASSERT_NE(slab32, nullptr);

    SegmentProvider::SegmentInfo info32 = sp.lookup(slab32);
    EXPECT_NE(info32.base, nullptr);
    EXPECT_GE(info32.size, sp.slab_size(SizeClass::k32B));

    sp.release_slab(slab32, SizeClass::k32B);
}

TEST(SegmentProviderTest, Stats) {
    SegmentProvider& sp = SegmentProvider::instance();

    // Acquire a slab to ensure stats are non-zero
    void* slab = sp.acquire_slab(SizeClass::k64B);
    ASSERT_NE(slab, nullptr);

    auto s = sp.stats();
    EXPECT_GT(s.active_segments, 0U);
    EXPECT_GT(s.total_allocated, 0U);

    sp.release_slab(slab, SizeClass::k64B);
}
