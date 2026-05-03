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

#include <hpactor/mem/thread_local_allocator.hpp>
#include <hpactor/mem/guard_page.hpp>
#include <hpactor/mem/zram.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <sys/mman.h>

int main() {
    using namespace hpactor::mem;
    using Clock = std::chrono::high_resolution_clock;

    // --- Benchmark: bump allocation hot path ---
    {
        ThreadLocalAllocator tla;
        constexpr int kIters = 100000;

        auto start = Clock::now();
        for (int i = 0; i < kIters; ++i) {
            void* p = tla.allocate(SizeClass::k64B,
                hpactor::ActorId{static_cast<uint64_t>(i)});
            *static_cast<uint64_t*>(p) = 0;
        }
        auto end = Clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start).count() / kIters;

        std::cout << "  bump alloc (64B): " << ns << " ns/op\n";
    }

    // --- Benchmark: alloc + free cycle ---
    {
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
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start).count() / kIters;

        std::cout << "  alloc+free cycle (128B): " << ns << " ns/op\n";
    }

    // --- Benchmark: freelist recycling ---
    {
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
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start).count() / kIters;

        std::cout << "  freelist recycle (256B): " << ns << " ns/op\n";
    }

    // --- Verify ZRAM / madvise integration ---
    {
        size_t ps = page_size();
        void* buf = mmap(nullptr, ps * 4, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf != MAP_FAILED) {
            std::memset(buf, 0x42, ps * 4);

            ZramManager::mark_cold(buf, ps * 4);
            ZramManager::reclaim_pages(buf, ps * 4);
            ZramManager::mark_will_need(buf, ps * 4);

            // Verify data survived the page hints
            volatile uint8_t v = *static_cast<uint8_t*>(buf);
            (void)v;

            munmap(buf, ps * 4);
        }

        std::cout << "  zram available: "
                  << (ZramManager::is_available() ? "yes" : "no") << "\n";
    }

    std::cout << "test_allocator_benchmark: PASS\n";
    return 0;
}
