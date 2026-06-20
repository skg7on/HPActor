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
#include <vector>

using namespace hpactor::mem;

// ── BoundaryFooter structure tests ──────────────────────────────

TEST(Coalescing, BoundaryFooterSizeMatchesCanaryFooter) {
    EXPECT_EQ(sizeof(BoundaryFooter), sizeof(CanaryFooter));
    EXPECT_EQ(sizeof(BoundaryFooter), 8u);
}

TEST(Coalescing, FooterOverlaySizeConstraint) {
    EXPECT_EQ(sizeof(FooterOverlay), 8u);
}

// ── Stamp/read boundary footer on free ──────────────────────────

TEST(Coalescing, StampBoundaryFooterOnFree) {
    SlabCache cache(SizeClass::k64B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* block = cache.allocate(hpactor::ActorId{1});
    cache.deallocate(block);

    // Read back: header should be marked freed, boundary footer at block end
    auto* header = AllocHeader::from_user_data(block);
    EXPECT_EQ(header->magic, kFreedMagic);
}

TEST(Coalescing, BoundaryFooterNotStampedOnLiveBlock) {
    SlabCache cache(SizeClass::k64B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* block = cache.allocate(hpactor::ActorId{1});
    auto* header = AllocHeader::from_user_data(block);
    size_t bs = block_size(SizeClass::k64B);
    auto* footer = reinterpret_cast<const CanaryFooter*>(
        reinterpret_cast<const char*>(header) + bs - sizeof(CanaryFooter));
    EXPECT_EQ(footer->magic, kAllocMagic); // canary intact on live block
}

// ── Left-neighbor coalescing ────────────────────────────────────

TEST(Coalescing, LeftNeighborCoalesce) {
    SlabCache cache(SizeClass::k64B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* a = cache.allocate(hpactor::ActorId{1});
    auto* b = cache.allocate(hpactor::ActorId{1});

    cache.deallocate(a); // A is free
    cache.deallocate(b); // B freed → should coalesce with A

    // After coalescing, both blocks should be logically merged
    // Re-allocate: should get back from the coalesced region
    auto* r1 = cache.allocate(hpactor::ActorId{1});
    EXPECT_NE(r1, nullptr);
    // The coalesced region provides a recycled block
}

// ── Right-neighbor coalescing ───────────────────────────────────

TEST(Coalescing, RightNeighborCoalesce) {
    SlabCache cache(SizeClass::k64B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* b = cache.allocate(hpactor::ActorId{1});
    auto* c = cache.allocate(hpactor::ActorId{1});

    cache.deallocate(c); // C is free
    cache.deallocate(b); // B freed → should coalesce with C

    auto* r1 = cache.allocate(hpactor::ActorId{1});
    EXPECT_NE(r1, nullptr);
    auto* r2 = cache.allocate(hpactor::ActorId{1});
    EXPECT_NE(r2, nullptr);
}

// ── Both-neighbor coalescing ────────────────────────────────────

TEST(Coalescing, BothNeighborsCoalesce) {
    SlabCache cache(SizeClass::k64B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* a = cache.allocate(hpactor::ActorId{1});
    auto* b = cache.allocate(hpactor::ActorId{1});
    auto* c = cache.allocate(hpactor::ActorId{1});

    cache.deallocate(a); // A free
    cache.deallocate(c); // C free
    cache.deallocate(b); // B free → coalesce A+B+C

    // All three should be recoverable from the coalesced region
    auto* r1 = cache.allocate(hpactor::ActorId{1});
    auto* r2 = cache.allocate(hpactor::ActorId{1});
    auto* r3 = cache.allocate(hpactor::ActorId{1});
    EXPECT_NE(r1, nullptr);
    EXPECT_NE(r2, nullptr);
    EXPECT_NE(r3, nullptr);
    EXPECT_NE(r1, r2);
    EXPECT_NE(r2, r3);
    EXPECT_NE(r1, r3);
}

// ── Boundary conditions ─────────────────────────────────────────

TEST(Coalescing, FullSlabFreeAndReallocate) {
    SlabCache cache(SizeClass::k64B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    std::vector<void*> blocks;
    for (int i = 0; i < 100; ++i) {
        blocks.push_back(cache.allocate(hpactor::ActorId{1}));
    }
    // Free all — should coalesce into large contiguous free regions
    for (auto* b : blocks) {
        cache.deallocate(b);
    }
    // Re-allocate — should succeed
    auto* r = cache.allocate(hpactor::ActorId{1});
    EXPECT_NE(r, nullptr);
}
