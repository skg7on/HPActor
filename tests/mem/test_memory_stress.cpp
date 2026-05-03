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
#include <hpactor/mem/size_class.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    using namespace hpactor::mem;
    using Clock = std::chrono::high_resolution_clock;

    constexpr int kThreads = 8;
    constexpr int kAllocsPerThread = 125000; // 1M total
    constexpr int kTotalAllocs = kThreads * kAllocsPerThread;

    std::atomic<uint64_t> alloc_count{0};
    std::atomic<uint64_t> free_count{0};

    auto start = Clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            ThreadLocalAllocator tla;
            std::vector<void*> ptrs;
            ptrs.reserve(kAllocsPerThread);

            // Allocate across all size classes with varied sizes
            for (int i = 0; i < kAllocsPerThread; ++i) {
                SizeClass sc;
                size_t user_sz;
                switch (i % 8) {
                    case 0: sc = SizeClass::k32B;  user_sz = 32;  break;
                    case 1: sc = SizeClass::k64B;  user_sz = 64;  break;
                    case 2: sc = SizeClass::k128B; user_sz = 128; break;
                    case 3: sc = SizeClass::k256B; user_sz = 256; break;
                    case 4: sc = SizeClass::k512B; user_sz = 512; break;
                    case 5: sc = SizeClass::k1KB;  user_sz = 1024; break;
                    case 6: sc = SizeClass::k2KB;  user_sz = 2048; break;
                    default: sc = SizeClass::k4KB; user_sz = 4096; break;
                }

                void* p = tla.allocate(sc,
                    hpactor::ActorId{static_cast<uint64_t>(t * kAllocsPerThread + i)});
                assert(p != nullptr);
                std::memset(p, static_cast<uint8_t>(i & 0xFF), user_sz);
                alloc_count.fetch_add(1);
                ptrs.push_back(p);
            }

            // Free in reverse order to stress freelist
            for (auto it = ptrs.rbegin(); it != ptrs.rend(); ++it) {
                tla.deallocate(*it);
                free_count.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = Clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();

    assert(alloc_count.load() == kTotalAllocs);
    assert(free_count.load() == kTotalAllocs);

    std::cout << "test_memory_stress: PASS (" << kTotalAllocs
              << " ops in " << elapsed_ms << " ms)\n";
    return 0;
}
