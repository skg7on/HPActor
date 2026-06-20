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
#include <hpactor/mem/super_carrier.hpp>

#include <cstring>

using namespace hpactor::mem;

TEST(SuperCarrier, InitReservesVirtualRange) {
    SuperCarrier carrier;
    bool ok = carrier.init(256ULL * 1024 * 1024); // 256MB
#ifdef __LP64__
    EXPECT_TRUE(ok);
    EXPECT_TRUE(carrier.is_initialized());
    EXPECT_EQ(carrier.total_reserved(), 256ULL * 1024 * 1024);
    EXPECT_EQ(carrier.total_carved(), 0u);
    EXPECT_EQ(carrier.current_carved(), 0u);
#else
    EXPECT_FALSE(ok) << "Super carrier disabled on 32-bit";
    EXPECT_FALSE(carrier.is_initialized());
#endif
}

TEST(SuperCarrier, CarveReturnsWritableMemory) {
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(128ULL * 1024 * 1024));
#ifdef __LP64__
    void* slab = carrier.carve(64 * 1024); // 64KB slab
    ASSERT_NE(slab, nullptr);
    std::memset(slab, 0x42, 64 * 1024);
    EXPECT_EQ(carrier.total_carved(), 64u * 1024);
    carrier.release(slab, 64 * 1024);
#endif
}

TEST(SuperCarrier, CarveAdvancesOffset) {
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(128ULL * 1024 * 1024));
#ifdef __LP64__
    void* s1 = carrier.carve(64 * 1024);
    void* s2 = carrier.carve(64 * 1024);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_NE(s1, s2);
    EXPECT_EQ(carrier.total_carved(), 128u * 1024);
    carrier.release(s1, 64 * 1024);
    carrier.release(s2, 64 * 1024);
#endif
}

TEST(SuperCarrier, ExhaustionReturnsNull) {
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(256 * 1024)); // tiny: only 256KB
#ifdef __LP64__
    // Carve until exhaustion
    void* s1 = carrier.carve(192 * 1024);
    ASSERT_NE(s1, nullptr);
    void* s2 = carrier.carve(64 * 1024);
    ASSERT_NE(s2, nullptr);
    // Next carve should fail
    void* s3 = carrier.carve(1 * 1024);
    EXPECT_EQ(s3, nullptr);
    carrier.release(s1, 192 * 1024);
    carrier.release(s2, 64 * 1024);
#endif
}

TEST(SuperCarrier, ReleaseReducesCurrentCarved) {
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(128ULL * 1024 * 1024));
#ifdef __LP64__
    void* slab = carrier.carve(128 * 1024);
    ASSERT_NE(slab, nullptr);
    EXPECT_EQ(carrier.current_carved(), 128u * 1024);
    carrier.release(slab, 128 * 1024);
    EXPECT_EQ(carrier.current_carved(), 0u);
#endif
}

TEST(SuperCarrier, ReleaseNonCarvedPointerIsSafe) {
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(128ULL * 1024 * 1024));
#ifdef __LP64__
    int dummy = 0;
    carrier.release(&dummy, sizeof(dummy)); // Should not crash
    SUCCEED();
#endif
}

TEST(SuperCarrier, InitZeroSizeFails) {
    SuperCarrier carrier;
    bool ok = carrier.init(0);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(carrier.is_initialized());
}

// ── MEM-005: Huge Page tests ───────────────────────────────────

TEST(HugePages, ProbeDoesNotCrash) {
    [[maybe_unused]] HugePageInfo info = probe_huge_pages();
    // Must not crash regardless of availability
    SUCCEED();
}

TEST(HugePages, ProbeSetsPageSizeWhenAvailable) {
    HugePageInfo info = probe_huge_pages();
    if (info.explicit_huge_pages_available) {
        EXPECT_GT(info.huge_page_size, 0u);
    }
}

TEST(HugePages, InitWithHugePageInfo) {
    HugePageInfo info = probe_huge_pages();
    SuperCarrier carrier;
    bool ok = carrier.init(256ULL * 1024 * 1024, info);
#ifdef __LP64__
    EXPECT_TRUE(ok);
    EXPECT_TRUE(carrier.is_initialized());
#endif
}

TEST(HugePages, InitWithNoHugePagesStillSucceeds) {
    HugePageInfo info{}; // all false
    SuperCarrier carrier;
    bool ok = carrier.init(128ULL * 1024 * 1024, info);
#ifdef __LP64__
    EXPECT_TRUE(ok);
#endif
}

// ── MEM-007: NUMA awareness tests ───────────────────────────────

TEST(NumaAware, ProbeTopologyDoesNotCrash) {
    NumaInfo info = probe_numa_topology();
    EXPECT_GE(info.node_count, 1u);
    EXPECT_LE(info.node_count, SuperCarrier::kMaxNumaNodes);
}

TEST(NumaAware, IsNumaWhenMultipleNodes) {
    NumaInfo info = probe_numa_topology();
    EXPECT_EQ(info.is_numa(), info.node_count > 1);
}

TEST(NumaAware, GetCurrentNumaNodeReturnsValid) {
    unsigned node = get_current_numa_node();
    (void)node;
    SUCCEED();
}

TEST(NumaAware, InitWithNumaInfo) {
    NumaInfo numa;
    numa.node_count = 4;
    SuperCarrier carrier;
    bool ok = carrier.init(256ULL * 1024 * 1024, HugePageInfo{}, numa);
#ifdef __LP64__
    EXPECT_TRUE(ok);
    EXPECT_TRUE(carrier.is_initialized());
#endif
}

TEST(NumaAware, CarveNumaSeparatesNodes) {
    NumaInfo numa;
    numa.node_count = 2;
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(256ULL * 1024 * 1024, HugePageInfo{}, numa));
#ifdef __LP64__
    void* n0 = carrier.carve_numa(64 * 1024, 0);
    ASSERT_NE(n0, nullptr);
    std::memset(n0, 0x0, 64 * 1024);

    void* n1 = carrier.carve_numa(64 * 1024, 1);
    ASSERT_NE(n1, nullptr);
    std::memset(n1, 0x11, 64 * 1024);

    const char* base = static_cast<const char*>(carrier.carrier_base());
    size_t off0 = static_cast<size_t>(static_cast<const char*>(n0) - base);
    size_t off1 = static_cast<size_t>(static_cast<const char*>(n1) - base);
    EXPECT_LT(off0, off1);

    carrier.release(n0, 64 * 1024);
    carrier.release(n1, 64 * 1024);
#endif
}

TEST(NumaAware, CarveNumaFallsBackOnExhaustion) {
    NumaInfo numa;
    numa.node_count = 2;
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(512 * 1024, HugePageInfo{}, numa));
#ifdef __LP64__
    void* n0 = carrier.carve_numa(96 * 1024, 0);
    ASSERT_NE(n0, nullptr);
    // node 0 exhausted — fallback to global
    void* n0b = carrier.carve_numa(96 * 1024, 0);
    ASSERT_NE(n0b, nullptr);
    carrier.release(n0, 96 * 1024);
    carrier.release(n0b, 96 * 1024);
#endif
}

TEST(NumaAware, CarveNumaBadNodeFallsBack) {
    NumaInfo numa;
    numa.node_count = 1;
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(128ULL * 1024 * 1024, HugePageInfo{}, numa));
#ifdef __LP64__
    void* slab = carrier.carve_numa(64 * 1024, 999);
    ASSERT_NE(slab, nullptr);
    carrier.release(slab, 64 * 1024);
#endif
}

TEST(NumaAware, SingleNodeIsNonNuma) {
    NumaInfo numa;
    numa.node_count = 1;
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(128ULL * 1024 * 1024, HugePageInfo{}, numa));
#ifdef __LP64__
    void* s1 = carrier.carve(64 * 1024);
    void* s2 = carrier.carve(64 * 1024);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_NE(s1, s2);
    carrier.release(s1, 64 * 1024);
    carrier.release(s2, 64 * 1024);
#endif
}
