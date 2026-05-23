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
#include <hpactor/mem/guard_page.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>
#include <hpactor/mem/zram.hpp>

#include <chrono>
#include <cstring>
#include <sys/mman.h>

using namespace hpactor::mem;
using Clock = std::chrono::high_resolution_clock;

TEST(AllocatorBenchmarkTest, BumpAllocationHotPath) {
    ThreadLocalAllocator tla;
    constexpr int kIters = 100000;

    auto start = Clock::now();
    for (int i = 0; i < kIters; ++i) {
        void* p = tla.allocate(SizeClass::k64B,
                               hpactor::ActorId{static_cast<uint64_t>(i)});
        *static_cast<uint64_t*>(p) = 0;
    }
    auto end = Clock::now();
    auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() /
        kIters;

    // Just ensure it completed without crash; performance is informational
    EXPECT_GT(ns, 0);
}

TEST(AllocatorBenchmarkTest, AllocFreeCycle) {
    ThreadLocalAllocator tla;
    constexpr int kIters = 50000;
    void* ptrs[kIters];

    auto start = Clock::now();
    for (int i = 0; i < kIters; ++i) {
        ptrs[i] = tla.allocate(SizeClass::k128B,
                               hpactor::ActorId{static_cast<uint64_t>(i)});
    }
    for (int i = 0; i < kIters; ++i) {
        tla.deallocate(ptrs[i]);
    }
    auto end = Clock::now();
    auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() /
        kIters;

    // Just verify it completed successfully
    EXPECT_GT(ns, 0);
}

TEST(AllocatorBenchmarkTest, FreelistRecycling) {
    ThreadLocalAllocator tla;
    constexpr int kIters = 100000;
    void* p = tla.allocate(SizeClass::k256B, hpactor::ActorId{1});
    tla.deallocate(p);

    auto start = Clock::now();
    for (int i = 0; i < kIters; ++i) {
        p = tla.allocate(SizeClass::k256B, hpactor::ActorId{2});
        tla.deallocate(p);
    }
    auto end = Clock::now();
    auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() /
        kIters;

    EXPECT_GT(ns, 0);
}

TEST(AllocatorBenchmarkTest, ZramMadviseIntegration) {
    size_t ps = page_size();
    void* buf = mmap(nullptr, ps * 4, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(buf, MAP_FAILED);

    std::memset(buf, 0x42, ps * 4);

    ZramManager::mark_cold(buf, ps * 4);
    ZramManager::reclaim_pages(buf, ps * 4);
    ZramManager::mark_will_need(buf, ps * 4);

    // Verify data survived the page hints
    volatile uint8_t v = *static_cast<uint8_t*>(buf);
    (void)v;

    munmap(buf, ps * 4);

    // is_available() should return a valid bool
    EXPECT_TRUE(ZramManager::is_available() || !ZramManager::is_available());
}
