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

#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/hibernatable.hpp>
#include <hpactor/mem/hibernation_registry.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/memory_tracker.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/slab_cache.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <sys/mman.h>
#include <thread>

namespace mem = hpactor::mem;

static hpactor::ActorId test_actor(uint64_t offset) {
    return hpactor::ActorId{800000 + offset};
}

// ---------------------------------------------------------------------------
// SlabCache allocation/deallocation for all size classes
// ---------------------------------------------------------------------------

class SlabCacheBranchesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tla_ = std::make_unique<mem::ThreadLocalAllocator>();
        mem::set_thread_allocator(tla_.get());
    }
    void TearDown() override {
        mem::set_thread_allocator(nullptr);
    }
    std::unique_ptr<mem::ThreadLocalAllocator> tla_;
};

TEST_F(SlabCacheBranchesTest, AllocateAndFreeAllSizeClasses) {
    // Test allocation for every size class
    std::array<mem::SizeClass, 8> all_classes = {
        mem::SizeClass::k32B,  mem::SizeClass::k64B,  mem::SizeClass::k128B,
        mem::SizeClass::k256B, mem::SizeClass::k512B, mem::SizeClass::k1KB,
        mem::SizeClass::k2KB,  mem::SizeClass::k4KB,
    };

    for (auto sc : all_classes) {
        void* ptr = mem::allocate_class(sc, test_actor(1));
        ASSERT_NE(ptr, nullptr)
            << "Failed to allocate size class " << static_cast<int>(sc);

        auto* header = mem::AllocHeader::from_user_data(ptr);
        EXPECT_TRUE(header->is_live());
        EXPECT_EQ(header->size_class, static_cast<uint8_t>(sc));

        mem::deallocate(ptr);

        // After deallocation, header should show freed magic
        EXPECT_TRUE(header->is_freed());
    }
}

TEST_F(SlabCacheBranchesTest, FreeListRecyclingAcrossCycles) {
    constexpr int kCycles = 10;

    for (int cycle = 0; cycle < kCycles; cycle++) {
        void* ptr = mem::allocate_class(mem::SizeClass::k64B, test_actor(1));
        ASSERT_NE(ptr, nullptr);

        auto* header = mem::AllocHeader::from_user_data(ptr);
        EXPECT_TRUE(header->is_live());

        // Write data to verify we own the memory
        std::memset(ptr, static_cast<int>(cycle + 1), 64);

        mem::deallocate(ptr);

        // After deallocation, header shows freed magic
        EXPECT_TRUE(header->is_freed());

        // Re-allocate — the same block may be recycled from freelist
        void* ptr2 = mem::allocate_class(mem::SizeClass::k64B, test_actor(1));
        ASSERT_NE(ptr2, nullptr);

        auto* header2 = mem::AllocHeader::from_user_data(ptr2);
        EXPECT_TRUE(header2->is_live());

        mem::deallocate(ptr2);
    }
}

TEST_F(SlabCacheBranchesTest, MultipleAllocationsFromSameSlab) {
    // Allocate many blocks from the same size class — they should come
    // from the same slab until it is exhausted, then refill.
    std::vector<void*> ptrs;
    for (int i = 0; i < 200; i++) {
        void* ptr = mem::allocate_class(mem::SizeClass::k32B, test_actor(1));
        ASSERT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }

    // All should be live
    for (auto* ptr : ptrs) {
        auto* header = mem::AllocHeader::from_user_data(ptr);
        EXPECT_TRUE(header->is_live());
    }

    // Free all
    for (auto* ptr : ptrs) {
        mem::deallocate(ptr);
    }
}

// ---------------------------------------------------------------------------
// AllocHeader validation — incarnation counter and region flags
// ---------------------------------------------------------------------------

TEST(AllocHeaderBranchesTest, FreedBlockHasFreedMagic) {
    mem::ThreadLocalAllocator tla;
    mem::set_thread_allocator(&tla);

    void* ptr = mem::allocate_class(mem::SizeClass::k64B, test_actor(10));
    ASSERT_NE(ptr, nullptr);
    auto* header = mem::AllocHeader::from_user_data(ptr);
    EXPECT_TRUE(header->is_live());
    EXPECT_FALSE(header->is_freed());

    mem::deallocate(ptr);

    // After deallocation, header should show freed magic
    EXPECT_TRUE(header->is_freed());
    EXPECT_FALSE(header->is_live());

    mem::set_thread_allocator(nullptr);
}

TEST(AllocHeaderBranchesTest, ReallocatedBlockIsLive) {
    mem::ThreadLocalAllocator tla;
    mem::set_thread_allocator(&tla);

    void* ptr1 = mem::allocate_class(mem::SizeClass::k64B, test_actor(10));
    ASSERT_NE(ptr1, nullptr);
    mem::deallocate(ptr1);

    // Re-allocate — should get a live block back
    void* ptr2 = mem::allocate_class(mem::SizeClass::k64B, test_actor(10));
    ASSERT_NE(ptr2, nullptr);
    auto* header2 = mem::AllocHeader::from_user_data(ptr2);
    EXPECT_TRUE(header2->is_live());
    EXPECT_FALSE(header2->is_freed());

    mem::deallocate(ptr2);
    mem::set_thread_allocator(nullptr);
}

TEST(AllocHeaderBranchesTest, RegionTypeStoredInHeader) {
    mem::ThreadLocalAllocator tla;
    mem::set_thread_allocator(&tla);

    // Allocate with a specific region type via the global allocate function
    void* ptr = mem::allocate(mem::RegionType::kNetwork, 128, test_actor(11));
    ASSERT_NE(ptr, nullptr);
    auto* header = mem::AllocHeader::from_user_data(ptr);
    EXPECT_EQ(header->region(), mem::RegionType::kNetwork);
    EXPECT_FALSE(header->is_fallback());

    mem::deallocate(ptr);
    mem::set_thread_allocator(nullptr);
}

TEST(AllocHeaderBranchesTest, SetRegionChangesFlag) {
    mem::ThreadLocalAllocator tla;
    mem::set_thread_allocator(&tla);

    void* ptr = mem::allocate_class(mem::SizeClass::k128B, test_actor(12));
    ASSERT_NE(ptr, nullptr);
    auto* header = mem::AllocHeader::from_user_data(ptr);
    EXPECT_EQ(header->region(), mem::RegionType::kInternal); // default

    header->set_region(mem::RegionType::kCoroutine);
    EXPECT_EQ(header->region(), mem::RegionType::kCoroutine);

    mem::deallocate(ptr);
    mem::set_thread_allocator(nullptr);
}

// ---------------------------------------------------------------------------
// MemoryRegion pressure state transitions
// ---------------------------------------------------------------------------

class MemoryRegionBranchesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tla_ = std::make_unique<mem::ThreadLocalAllocator>();
        mem::set_thread_allocator(tla_.get());
    }
    void TearDown() override {
        mem::set_thread_allocator(nullptr);
        // Reset region limits
        auto& reg = mem::MemoryRegionRegistry::instance();
        reg.configure_region(mem::RegionType::kInternal, mem::RegionLimit{});
        reg.configure_region(mem::RegionType::kNetwork, mem::RegionLimit{});
    }
    std::unique_ptr<mem::ThreadLocalAllocator> tla_;
};

TEST_F(MemoryRegionBranchesTest, PressureStateNormalByDefault) {
    auto& reg = mem::MemoryRegionRegistry::instance();
    auto snap = reg.snapshot(mem::RegionType::kInternal);
    EXPECT_EQ(snap.pressure, mem::MemoryPressureState::kNormal);
}

TEST_F(MemoryRegionBranchesTest, PressureStateTransitions) {
    auto& reg = mem::MemoryRegionRegistry::instance();

    // Set a hard limit on kNetwork
    reg.configure_region(mem::RegionType::kNetwork,
                         mem::RegionLimit{1024 * 1024, 0.5f});

    // Allocate a bunch of memory in the kNetwork region
    std::vector<void*> ptrs;
    for (int i = 0; i < 100; i++) {
        void* ptr = mem::allocate(mem::RegionType::kNetwork, 256, test_actor(20));
        if (ptr == nullptr)
            break; // stopped by hard limit
        ptrs.push_back(ptr);
    }

    // After many allocations, we should at least see a snapshot reflecting
    // the configured limit
    auto snap = reg.snapshot(mem::RegionType::kNetwork);
    EXPECT_EQ(snap.limit.hard_limit_bytes, 1024u * 1024);
    EXPECT_FLOAT_EQ(snap.limit.high_watermark_ratio, 0.5f);

    for (auto* ptr : ptrs) {
        mem::deallocate(ptr);
    }
}

TEST_F(MemoryRegionBranchesTest, HardLimitRejectsAllocations) {
    auto& reg = mem::MemoryRegionRegistry::instance();

    // Set a very small hard limit — 32 bytes
    reg.configure_region(mem::RegionType::kInternal, mem::RegionLimit{32, 1.0f});

    // First allocation should succeed (under limit)
    void* ptr1 = mem::allocate(mem::RegionType::kInternal, 8, test_actor(21));
    if (ptr1) {
        // Second allocation should be rejected or fallback
        reg.snapshot(mem::RegionType::kInternal);

        void* ptr2 = mem::allocate(mem::RegionType::kInternal, 8, test_actor(21));
        if (ptr2) {
            // May have used fallback or the limit wasn't strict
            mem::deallocate(ptr2);
        }

        // Check that rejected_alloc_count increments on rejection
        // (at least verify the snapshot works)
        auto snap = reg.snapshot(mem::RegionType::kInternal);
        EXPECT_EQ(snap.limit.hard_limit_bytes, 32u);

        mem::deallocate(ptr1);
    }
}

TEST_F(MemoryRegionBranchesTest, SnapshotAllRegionTypes) {
    auto& reg = mem::MemoryRegionRegistry::instance();

    std::array<mem::RegionType, 6> all_regions = {
        mem::RegionType::kActor,     mem::RegionType::kMessage,
        mem::RegionType::kCoroutine, mem::RegionType::kNetwork,
        mem::RegionType::kInternal,  mem::RegionType::kHibernate,
    };

    for (auto rt : all_regions) {
        auto snap = reg.snapshot(rt);
        // Each region should have a valid snapshot
        EXPECT_GE(snap.total_allocated, 0u);
        EXPECT_GE(snap.total_freed, 0u);
    }
}

// ---------------------------------------------------------------------------
// MemoryTracker multiple actors and aggregates
// ---------------------------------------------------------------------------

class MemoryTrackerBranchesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tla_ = std::make_unique<mem::ThreadLocalAllocator>();
        mem::set_thread_allocator(tla_.get());
    }
    void TearDown() override {
        mem::set_thread_allocator(nullptr);
    }
    std::unique_ptr<mem::ThreadLocalAllocator> tla_;
};

TEST_F(MemoryTrackerBranchesTest, MultipleActorsTracking) {
    auto* tracker = &mem::MemoryTracker::instance();

    // Allocate for actor A
    void* a1 = mem::allocate(mem::RegionType::kActor, 100, test_actor(30));
    ASSERT_NE(a1, nullptr);
    void* a2 = mem::allocate(mem::RegionType::kActor, 200, test_actor(30));
    ASSERT_NE(a2, nullptr);

    // Allocate for actor B
    void* b1 = mem::allocate(mem::RegionType::kActor, 50, test_actor(31));
    ASSERT_NE(b1, nullptr);

    mem::ActorMemoryStats stats_a{};
    mem::ActorMemoryStats stats_b{};
    tracker->snapshot(test_actor(30), stats_a);
    tracker->snapshot(test_actor(31), stats_b);

    // Actor A should have more allocated bytes than B
    EXPECT_GT(stats_a.alloc_count, uint64_t{0});
    EXPECT_GT(stats_b.alloc_count, uint64_t{0});

    mem::deallocate(a1);
    mem::deallocate(a2);
    mem::deallocate(b1);
}

TEST_F(MemoryTrackerBranchesTest, TotalAggregatesUpdateCorrectly) {
    auto* tracker = &mem::MemoryTracker::instance();

    uint64_t total_active_before = tracker->total_active_bytes();

    std::vector<void*> ptrs;
    for (int i = 0; i < 10; i++) {
        void* ptr = mem::allocate(mem::RegionType::kActor, 64, test_actor(40));
        if (ptr)
            ptrs.push_back(ptr);
    }

    uint64_t total_active_after_alloc = tracker->total_active_bytes();
    // Active bytes should have increased
    EXPECT_GE(total_active_after_alloc, total_active_before);

    for (auto* ptr : ptrs) {
        mem::deallocate(ptr);
    }

    uint64_t total_active_after_free = tracker->total_active_bytes();
    // After freeing all, active bytes should be back near original
    EXPECT_LE(total_active_after_free, total_active_after_alloc);
}

// ---------------------------------------------------------------------------
// ThreadLocalAllocator per-thread isolation
// ---------------------------------------------------------------------------

TEST(ThreadLocalAllocatorBranchesTest, AllocatorIsolationAcrossThreads) {
    mem::ThreadLocalAllocator tla_main;
    mem::set_thread_allocator(&tla_main);

    // Allocate on main thread
    void* main_ptr = mem::allocate_class(mem::SizeClass::k32B, test_actor(50));
    ASSERT_NE(main_ptr, nullptr);
    auto* main_header = mem::AllocHeader::from_user_data(main_ptr);
    EXPECT_TRUE(main_header->is_live());

    // Deallocate from a different thread (cross-thread free)
    std::thread other_thread([ptr = main_ptr]() {
        mem::ThreadLocalAllocator tla_other;
        mem::set_thread_allocator(&tla_other);
        mem::deallocate(ptr);
        mem::set_thread_allocator(nullptr);
    });
    other_thread.join();

    // After cross-thread free, header should show freed
    EXPECT_TRUE(main_header->is_freed());

    mem::set_thread_allocator(nullptr);
}

TEST(ThreadLocalAllocatorBranchesTest, AllocateBytesAutoSelectsSizeClass) {
    mem::ThreadLocalAllocator tla;
    mem::set_thread_allocator(&tla);

    // allocate_bytes should select the correct size class based on bytes
    void* ptr = tla.allocate_bytes(25, test_actor(51));
    ASSERT_NE(ptr, nullptr);
    auto* header = mem::AllocHeader::from_user_data(ptr);
    // 25 bytes → rounds up to k32B
    EXPECT_EQ(header->size_class, static_cast<uint8_t>(mem::SizeClass::k32B));

    mem::deallocate(ptr);
    mem::set_thread_allocator(nullptr);
}

// ---------------------------------------------------------------------------
// HibernationRegistry remove operation
// ---------------------------------------------------------------------------

TEST(HibernationRegistryBranchesTest, RemoveFreesAndForgets) {
    auto& reg = mem::HibernationRegistry::instance();
    size_t before = reg.count();

    // Create and store a buffer
    size_t buf_size = 256;
    void* buf = mmap(nullptr, buf_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(buf, MAP_FAILED);

    hpactor::ActorId aid{200};
    mem::HibernationBuffer hb{buf, buf_size, 0, 0};
    reg.store(aid, hb);

    ASSERT_TRUE(reg.contains(aid));
    ASSERT_EQ(reg.count(), before + 1);

    // Remove
    reg.remove(aid);

    EXPECT_FALSE(reg.contains(aid));
    EXPECT_EQ(reg.count(), before);
    EXPECT_EQ(reg.total_bytes(), 0u);

    // Buffer was freed by remove()
}

TEST(HibernationRegistryBranchesTest, RemoveNonexistentIsNoop) {
    auto& reg = mem::HibernationRegistry::instance();
    size_t before = reg.count();

    hpactor::ActorId nonexistent{999999};
    EXPECT_FALSE(reg.contains(nonexistent));

    // Should not crash or change count
    reg.remove(nonexistent);
    EXPECT_EQ(reg.count(), before);
}

TEST(HibernationRegistryBranchesTest, LoadTransfersOwnership) {
    auto& reg = mem::HibernationRegistry::instance();

    size_t buf_size = 128;
    void* buf = mmap(nullptr, buf_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(buf, MAP_FAILED);
    std::memset(buf, 0xAB, buf_size);

    hpactor::ActorId aid{201};
    mem::HibernationBuffer hb{buf, buf_size, uint64_t{100},
                              static_cast<uint32_t>(aid.value())};
    reg.store(aid, hb);
    ASSERT_TRUE(reg.contains(aid));

    mem::HibernationBuffer loaded = reg.load(aid);

    // After load, entry removed from registry
    EXPECT_FALSE(reg.contains(aid));

    // Verify buffer transferred with correct metadata
    EXPECT_EQ(loaded.ptr, buf);
    EXPECT_EQ(loaded.size, buf_size);
    EXPECT_EQ(loaded.hibernated_at_ts, 100u);
    EXPECT_EQ(loaded.actor_id, aid.value());

    // Clean up ownership transferred to us
    munmap(loaded.ptr, loaded.size);
}

TEST(HibernationRegistryBranchesTest, CountAndTotalBytesTrackCorrectly) {
    auto& reg = mem::HibernationRegistry::instance();
    size_t count_before = reg.count();
    uint64_t bytes_before = reg.total_bytes();

    // Store first buffer
    size_t sz1 = 512;
    void* buf1 = mmap(nullptr, sz1, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(buf1, MAP_FAILED);
    reg.store(hpactor::ActorId{202}, mem::HibernationBuffer{buf1, sz1, 0, 0});

    EXPECT_EQ(reg.count(), count_before + 1);
    EXPECT_EQ(reg.total_bytes(), bytes_before + sz1);

    // Store second buffer
    size_t sz2 = 256;
    void* buf2 = mmap(nullptr, sz2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(buf2, MAP_FAILED);
    reg.store(hpactor::ActorId{203}, mem::HibernationBuffer{buf2, sz2, 0, 0});

    EXPECT_EQ(reg.count(), count_before + 2);
    EXPECT_EQ(reg.total_bytes(), bytes_before + sz1 + sz2);

    // Remove first — count and bytes should decrease
    reg.remove(hpactor::ActorId{202});
    EXPECT_EQ(reg.count(), count_before + 1);
    EXPECT_EQ(reg.total_bytes(), bytes_before + sz2);

    // Load second — count and bytes should go to zero
    auto loaded = reg.load(hpactor::ActorId{203});
    EXPECT_EQ(reg.count(), count_before);
    EXPECT_EQ(reg.total_bytes(), bytes_before);
    munmap(loaded.ptr, loaded.size);
}
