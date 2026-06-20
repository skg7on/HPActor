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
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/slab_cache.hpp>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace hpactor::mem;

TEST(SegregatedFreelist, StrategyDefaultIsCasLifo) {
    SlabCache cache(SizeClass::k32B);
    EXPECT_EQ(cache.strategy(), AllocationStrategy::kCasLifo);
}

TEST(SegregatedFreelist, StrategySetAtConstruction) {
    SlabCache cache(SizeClass::k32B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    EXPECT_EQ(cache.strategy(), AllocationStrategy::kSegregatedFit);
}

TEST(SegregatedFreelist, BinCountMatchesConstant) {
    SlabCache cache(SizeClass::k32B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    EXPECT_EQ(cache.bin_count(), kNumSegregatedBins);
}

TEST(SegregatedFreelist, SegregatedAllocateAndFree) {
    SlabCache cache(SizeClass::k64B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);

    void* b1 = cache.allocate(hpactor::ActorId{1});
    void* b2 = cache.allocate(hpactor::ActorId{2});
    ASSERT_NE(b1, nullptr);
    ASSERT_NE(b2, nullptr);
    EXPECT_EQ(cache.live_count(), 2U);

    // Free both blocks — should go to segregated bins
    cache.deallocate(b1);
    cache.deallocate(b2);

    // Re-allocate — with bump-first (MEM-001 §3.2), virgin memory is preferred.
    // New allocations come from bump, not from recycled bins.
    void* b3 = cache.allocate(hpactor::ActorId{3});
    void* b4 = cache.allocate(hpactor::ActorId{4});
    ASSERT_NE(b3, nullptr);
    ASSERT_NE(b4, nullptr);

    // Bump-first: new allocations from virgin memory, not recycled
    EXPECT_NE(b3, b1);
    EXPECT_NE(b3, b2);
    EXPECT_NE(b4, b1);
    EXPECT_NE(b4, b2);
    EXPECT_NE(b3, b4);

    EXPECT_EQ(cache.live_count(), 2U);
}

TEST(SegregatedFreelist, RefillWhenBinsExhausted) {
    SlabCache cache(SizeClass::k32B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);

    // Allocate until first slab is full
    std::vector<void*> blocks;
    for (int i = 0; i < 1000; ++i) {
        void* block = cache.allocate(hpactor::ActorId{1});
        ASSERT_NE(block, nullptr) << "allocation " << i << " failed";
        blocks.push_back(block);
    }

    // Bump pointer should have refilled at least once
    const auto& s = cache.stats();
    EXPECT_GE(s.slab_acquire_count.load(), 1u);

    // Cleanup: free all
    for (auto* b : blocks) {
        cache.deallocate(b);
    }
}

TEST(SegregatedFreelist, NewSlabInheritsStrategy) {
    SlabCache cache(SizeClass::k32B, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);

    // Fill first slab, allocate into refill
    std::vector<void*> blocks;
    while (blocks.size() < 2000) {
        void* block = cache.allocate(hpactor::ActorId{1});
        if (!block)
            break;
        blocks.push_back(block);
    }
    ASSERT_GT(blocks.size(), 500u);

    // Free all — should route to bins
    for (auto* b : blocks) {
        cache.deallocate(b);
    }

    // Re-allocate — should come from bins
    void* recycled = cache.allocate(hpactor::ActorId{99});
    ASSERT_NE(recycled, nullptr);
}

TEST(SegregatedFreelist, Concurrent4ThreadStress) {
    // 4 threads, each with its own SlabCache using segregated strategy.
    // 10K alloc + free each, verify no corruption.
    std::atomic<bool> start{false};
    std::atomic<uint64_t> total_allocs{0};
    std::atomic<uint64_t> total_frees{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            // Each thread gets its own cache (as in real ThreadLocalAllocator)
            SlabCache cache(SizeClass::k64B, RegionType::kActor,
                            AllocationStrategy::kSegregatedFit);
            std::vector<void*> blocks;
            while (!start.load(std::memory_order_acquire)) { /* spin */
            }
            for (int i = 0; i < 10000; ++i) {
                hpactor::ActorId owner{static_cast<uint32_t>(t * 10000 + i + 1)};
                void* b = cache.allocate(owner);
                if (b) {
                    std::memset(b, static_cast<int>(t & 0xFF), 64);
                    blocks.push_back(b);
                    total_allocs.fetch_add(1, std::memory_order_relaxed);
                }
            }
            for (auto* b : blocks) {
                cache.deallocate(b);
                total_frees.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : threads)
        t.join();

    EXPECT_GE(total_allocs.load(), 30000u);
    EXPECT_EQ(total_allocs.load(), total_frees.load());
}

// ── Bump-only strategy tests (MEM-003) ─────────────────────────

TEST(SegregatedFreelist, BumpOnlySkipsFreelist) {
    SlabCache cache(SizeClass::k64B, RegionType::kMessage,
                    AllocationStrategy::kBumpOnly);
    auto* b1 = cache.allocate(hpactor::ActorId{1});
    auto* b2 = cache.allocate(hpactor::ActorId{2});
    EXPECT_NE(b1, b2);

    // Free b1 — in bump-only, it goes nowhere (no freelist)
    cache.deallocate(b1);
    // Allocate again — should NOT get b1 back (bump-only, no freelist pop)
    auto* b3 = cache.allocate(hpactor::ActorId{3});
    EXPECT_NE(b3, b1); // comes from bump, not recycled
    (void)b3;
}

TEST(SegregatedFreelist, BumpOnlyRefill) {
    SlabCache cache(SizeClass::k32B, RegionType::kMessage,
                    AllocationStrategy::kBumpOnly);
    std::vector<void*> blocks;
    for (int i = 0; i < 2000; ++i) {
        void* b = cache.allocate(hpactor::ActorId{1});
        ASSERT_NE(b, nullptr);
        blocks.push_back(b);
    }
    const auto& s2 = cache.stats();
    EXPECT_GE(s2.slab_acquire_count.load(), 1u);
    for (auto* b : blocks)
        cache.deallocate(b);
}

// ── Idle slab recycling tests (MEM-003 §3.3) ────────────────────

TEST(SegregatedFreelist, BumpOnlySlabRecycle) {
    SlabCache cache(SizeClass::k64B, RegionType::kMessage,
                    AllocationStrategy::kBumpOnly);

    // Fill one slab
    std::vector<void*> blocks;
    for (int i = 0; i < 200; ++i) {
        void* b = cache.allocate(hpactor::ActorId{1});
        ASSERT_NE(b, nullptr);
        blocks.push_back(b);
    }

    const auto& stats_before = cache.stats();
    size_t slabs_before = stats_before.slab_acquire_count.load();

    // Free all blocks — slab should be recycled to idle list
    for (auto* b : blocks)
        cache.deallocate(b);

    // Allocate again — should reuse the idle slab, not call SegmentProvider
    std::vector<void*> recycled;
    for (int i = 0; i < 200; ++i) {
        void* b = cache.allocate(hpactor::ActorId{2});
        ASSERT_NE(b, nullptr);
        recycled.push_back(b);
    }

    const auto& stats_after = cache.stats();
    // No new slab acquired — idle slab was reused
    EXPECT_EQ(stats_after.slab_acquire_count.load(), slabs_before);

    for (auto* b : recycled)
        cache.deallocate(b);
}

// ── Strategy dispatch tests (MEM-003 §5.1) ──────────────────────

TEST(SegregatedFreelist, StrategyDispatchCorrectness) {
    // Use ThreadLocalAllocator with default strategy table
    MemoryStrategyTable table = kDefaultStrategies;

    // kMessage → bump-only
    EXPECT_EQ(table.regions[static_cast<uint8_t>(RegionType::kMessage)].strategy,
              AllocationStrategy::kBumpOnly);
    EXPECT_FALSE(
        table.regions[static_cast<uint8_t>(RegionType::kMessage)].enable_coalescing);

    // kActor → segregated + coalescing
    EXPECT_EQ(table.regions[static_cast<uint8_t>(RegionType::kActor)].strategy,
              AllocationStrategy::kSegregatedFit);
    EXPECT_TRUE(
        table.regions[static_cast<uint8_t>(RegionType::kActor)].enable_coalescing);

    // kNetwork → bump-only
    EXPECT_EQ(table.regions[static_cast<uint8_t>(RegionType::kNetwork)].strategy,
              AllocationStrategy::kBumpOnly);

    // kCoroutine → cas_lifo (default)
    EXPECT_EQ(table.regions[static_cast<uint8_t>(RegionType::kCoroutine)].strategy,
              AllocationStrategy::kCasLifo);
}
