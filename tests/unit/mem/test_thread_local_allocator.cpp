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
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>

#include <cstring>
#include <thread>
#include <vector>

using namespace hpactor::mem;

TEST(ThreadLocalAllocatorTest, AllocateDifferentSizeClasses) {
    ThreadLocalAllocator tla;

    void* p32 = tla.allocate(SizeClass::k32B, hpactor::ActorId{1});
    void* p256 = tla.allocate(SizeClass::k256B, hpactor::ActorId{2});
    void* p1k = tla.allocate(SizeClass::k1KB, hpactor::ActorId{3});

    ASSERT_NE(p32, nullptr);
    ASSERT_NE(p256, nullptr);
    ASSERT_NE(p1k, nullptr);

    std::memset(p32, 0xAA, 32);
    std::memset(p256, 0xBB, 256);
    std::memset(p1k, 0xCC, 1024);

    tla.deallocate(p32);
    tla.deallocate(p256);
    tla.deallocate(p1k);
}

TEST(ThreadLocalAllocatorTest, AllocateBytesAutoSelectsSizeClass) {
    ThreadLocalAllocator tla;

    void* p50 = tla.allocate_bytes(50, hpactor::ActorId{4}); // -> 64B
    ASSERT_NE(p50, nullptr);
    std::memset(p50, 0xDD, 50);

    tla.deallocate(p50);
}

TEST(ThreadLocalAllocatorTest, ConcurrentAllocationMultipleThreads) {
    constexpr int kThreads = 4;
    constexpr int kAllocsPerThread = 1000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t]() {
            ThreadLocalAllocator tla;
            std::vector<void*> ptrs;
            for (int i = 0; i < kAllocsPerThread; ++i) {
                void* p = tla.allocate(SizeClass::k64B,
                                       hpactor::ActorId{static_cast<uint64_t>(
                                           t * kAllocsPerThread + i)});
                ASSERT_NE(p, nullptr);
                std::memset(p, static_cast<uint8_t>(t), 64);
                ptrs.push_back(p);
            }
            for (auto* p : ptrs) {
                tla.deallocate(p);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
}
