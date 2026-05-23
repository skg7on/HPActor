// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/memory_tracker.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <thread>

namespace mem = hpactor::mem;

static hpactor::ActorId test_actor(uint64_t offset) {
    return hpactor::ActorId{900000 + offset};
}

static mem::ActorMemoryStats actor_stats(hpactor::ActorId id) {
    mem::ActorMemoryStats out{};
    mem::MemoryTracker::instance().snapshot(id, out);
    return out;
}

class MemoryRegionAccountingTest : public ::testing::Test {
  protected:
    void TearDown() override {
        mem::set_thread_allocator(nullptr);
        mem::MemoryRegionRegistry::instance().configure_region(
            mem::RegionType::kNetwork, mem::RegionLimit{});
    }
};

TEST_F(MemoryRegionAccountingTest, RegionMetadataAndAccounting) {
    mem::ThreadLocalAllocator tla;
    mem::set_thread_allocator(&tla);

    auto& regions = mem::MemoryRegionRegistry::instance();
    const auto before_region = regions.snapshot(mem::RegionType::kMessage);
    const auto before_actor = actor_stats(test_actor(1));

    void* ptr = mem::allocate(mem::RegionType::kMessage, 100, test_actor(1));
    ASSERT_NE(ptr, nullptr);

    auto* header = mem::AllocHeader::from_user_data(ptr);
    EXPECT_EQ(header->region(), mem::RegionType::kMessage);
    EXPECT_FALSE(header->is_fallback());
    EXPECT_EQ(header->user_size(), 128u);

    const auto during_region = regions.snapshot(mem::RegionType::kMessage);
    const auto during_actor = actor_stats(test_actor(1));
    EXPECT_EQ(during_region.active_bytes, before_region.active_bytes + 128);
    EXPECT_EQ(during_region.alloc_count, before_region.alloc_count + 1);
    EXPECT_EQ(during_actor.current_bytes, before_actor.current_bytes + 128);
    EXPECT_EQ(during_actor.alloc_count, before_actor.alloc_count + 1);

    mem::deallocate(ptr);

    const auto after_region = regions.snapshot(mem::RegionType::kMessage);
    const auto after_actor = actor_stats(test_actor(1));
    EXPECT_EQ(after_region.active_bytes, before_region.active_bytes);
    EXPECT_EQ(after_region.free_count, before_region.free_count + 1);
    EXPECT_EQ(after_actor.current_bytes, before_actor.current_bytes);
    EXPECT_EQ(after_actor.free_count, before_actor.free_count + 1);
}

TEST_F(MemoryRegionAccountingTest, CrossThreadFreeReturnsToOriginCache) {
    mem::ThreadLocalAllocator producer;
    mem::ThreadLocalAllocator consumer;

    mem::set_thread_allocator(&producer);
    const auto before_free =
        producer.stats(mem::RegionType::kMessage, mem::SizeClass::k32B)
            .free_count.load();

    void* ptr = mem::allocate(mem::RegionType::kMessage, 32, test_actor(2));
    ASSERT_NE(ptr, nullptr);

    mem::set_thread_allocator(&consumer);
    mem::deallocate(ptr);

    const auto after_free =
        producer.stats(mem::RegionType::kMessage, mem::SizeClass::k32B)
            .free_count.load();
    EXPECT_EQ(after_free, before_free + 1);
}

TEST_F(MemoryRegionAccountingTest, FallbackAllocationIsSelfDescribing) {
    mem::set_thread_allocator(nullptr);

    void* ptr = mem::allocate(mem::RegionType::kNetwork, 48, test_actor(3));
    ASSERT_NE(ptr, nullptr);

    auto* header = mem::AllocHeader::from_user_data(ptr);
    EXPECT_EQ(header->region(), mem::RegionType::kNetwork);
    EXPECT_TRUE(header->is_fallback());
    EXPECT_EQ(header->user_size(), 64u);

    mem::ThreadLocalAllocator freeing_thread;
    mem::set_thread_allocator(&freeing_thread);
    mem::deallocate(ptr);
}

TEST_F(MemoryRegionAccountingTest, RegionHardLimitRejectsBeforeSlabGrowth) {
    mem::ThreadLocalAllocator tla;
    mem::set_thread_allocator(&tla);

    auto& regions = mem::MemoryRegionRegistry::instance();
    const auto before = regions.snapshot(mem::RegionType::kNetwork);

    mem::RegionLimit limit{};
    limit.hard_limit_bytes = before.active_bytes + 64;
    limit.high_watermark_ratio = 0.50f;
    regions.configure_region(mem::RegionType::kNetwork, limit);

    void* first = mem::allocate(mem::RegionType::kNetwork, 64, test_actor(4));
    ASSERT_NE(first, nullptr);

    void* rejected = mem::allocate(mem::RegionType::kNetwork, 64, test_actor(4));
    EXPECT_EQ(rejected, nullptr);

    const auto after_reject = regions.snapshot(mem::RegionType::kNetwork);
    EXPECT_EQ(after_reject.rejected_alloc_count, before.rejected_alloc_count + 1);
    EXPECT_EQ(after_reject.active_bytes, before.active_bytes + 64);

    mem::deallocate(first);
    regions.configure_region(mem::RegionType::kNetwork, mem::RegionLimit{});
}
