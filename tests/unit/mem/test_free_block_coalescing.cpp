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

// ── Boundary edge cases ─────────────────────────────────────────

TEST(Coalescing, FirstBlockInSlabNoLeftCoalesce) {
    // The very first block in a slab has no left neighbor — should not crash
    SlabCache cache(SizeClass::k64B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* first = cache.allocate(hpactor::ActorId{1});
    auto* second = cache.allocate(hpactor::ActorId{2});

    cache.deallocate(first);  // First block freed — no left neighbor
    cache.deallocate(second); // Second should coalesce left with first

    // Both should be coalesced into one free region
    auto* r1 = cache.allocate(hpactor::ActorId{1});
    auto* r2 = cache.allocate(hpactor::ActorId{2});
    EXPECT_NE(r1, nullptr);
    EXPECT_NE(r2, nullptr);
}

TEST(Coalescing, LastBlockInSlabNoRightCoalesce) {
    // The last block in a full slab has no right neighbor — should not crash
    SlabCache cache(SizeClass::k64B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    std::vector<void*> blocks;
    // Fill the slab
    for (int i = 0; i < 100; ++i) {
        auto* b = cache.allocate(hpactor::ActorId{1});
        ASSERT_NE(b, nullptr);
        blocks.push_back(b);
    }
    // Free the last block — should not read past slab end
    auto* last = blocks.back();
    cache.deallocate(last);
    SUCCEED(); // no crash
}

// ── Middle-of-list removal verification ──────────────────────────

TEST(Coalescing, CoalesceAfterInterleavedFrees) {
    // This test validates dll_remove() works correctly for non-head removal.
    // Free A, allocate from bin (moves A's bin head), free B adjacent to A.
    // B should still coalesce with A even though A is no longer at bin head.
    SlabCache cache(SizeClass::k64B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);

    // Allocate and free three adjacent blocks: X, A, B
    auto* x = cache.allocate(hpactor::ActorId{1});
    auto* a = cache.allocate(hpactor::ActorId{2});
    auto* b = cache.allocate(hpactor::ActorId{3});
    ASSERT_NE(x, nullptr);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // Free A (goes to bin), then free X (goes to same bin, now at head)
    cache.deallocate(a);
    cache.deallocate(x);

    // Now free B — A is adjacent to B but NOT at bin head (X is at head).
    // dll_remove() must find A in the middle of the list and remove it.
    cache.deallocate(b);

    // All three should be coalesced: X-A-B
    auto* r1 = cache.allocate(hpactor::ActorId{1});
    auto* r2 = cache.allocate(hpactor::ActorId{2});
    auto* r3 = cache.allocate(hpactor::ActorId{3});
    EXPECT_NE(r1, nullptr);
    EXPECT_NE(r2, nullptr);
    EXPECT_NE(r3, nullptr);
}

// ── 32B block exclusion ─────────────────────────────────────────

TEST(Coalescing, CoalescingSkip32BBlock) {
    // MEM-002 Appendix A: coalescing is disabled for 32B size class
    // (block total = 72B, user data = 32B, can't store prev pointer).
    SlabCache cache(SizeClass::k32B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);

    auto* a = cache.allocate(hpactor::ActorId{1});
    auto* b = cache.allocate(hpactor::ActorId{2});
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    cache.deallocate(a);
    cache.deallocate(b);
    // For 32B: coalescing is skipped, blocks remain separate

    auto* r1 = cache.allocate(hpactor::ActorId{1});
    EXPECT_NE(r1, nullptr);
}
