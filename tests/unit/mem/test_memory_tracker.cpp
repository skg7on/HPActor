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

#include <hpactor/mem/memory_tracker.hpp>

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace hpactor::mem;

// Tests that depend on MemoryTracker singleton state are combined into a single
// test to avoid execution-order dependencies.
TEST(MemoryTrackerTest, RecordAllocFreeAndGlobalAggregates) {
    auto& mt = MemoryTracker::instance();
    ActorMemoryStats snap;

    // Record allocations for several actors
    EXPECT_TRUE(mt.record_alloc(hpactor::ActorId{1}, 128));
    EXPECT_TRUE(mt.record_alloc(hpactor::ActorId{1}, 256));
    EXPECT_TRUE(mt.record_alloc(hpactor::ActorId{2}, 512));

    // Check per-actor snapshots
    mt.snapshot(hpactor::ActorId{1}, snap);
    EXPECT_EQ(snap.current_bytes, 384u); // 128 + 256
    EXPECT_EQ(snap.peak_bytes, 384u);
    EXPECT_EQ(snap.alloc_count, 2u);

    mt.snapshot(hpactor::ActorId{2}, snap);
    EXPECT_EQ(snap.current_bytes, 512u);
    EXPECT_EQ(snap.alloc_count, 1u);

    // Record frees
    mt.record_free(hpactor::ActorId{1}, 128);
    mt.snapshot(hpactor::ActorId{1}, snap);
    EXPECT_EQ(snap.current_bytes, 256u); // 384 - 128
    EXPECT_EQ(snap.free_count, 1u);
    EXPECT_EQ(snap.peak_bytes, 384u); // peak unchanged

    // Global aggregates
    EXPECT_EQ(mt.total_active_bytes(), 768u); // 256 + 512
    EXPECT_EQ(mt.total_alloc_count(), 3u);
}

TEST(MemoryTrackerTest, ConcurrentTrackingFromMultipleThreads) {
    auto& mt = MemoryTracker::instance();

    constexpr int kThreads = 4;
    constexpr int kAllocsPerThread = 1000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&mt, t]() {
            for (int i = 0; i < kAllocsPerThread; ++i) {
                mt.record_alloc(hpactor::ActorId{static_cast<uint64_t>(t + 1000)},
                                static_cast<size_t>(64));
            }
        });
    }
    for (auto& th : threads)
        th.join();

    // Verify no data corruption from concurrent access
    ActorMemoryStats snap;
    for (int t = 0; t < kThreads; ++t) {
        mt.snapshot(hpactor::ActorId{static_cast<uint64_t>(t + 1000)}, snap);
        EXPECT_EQ(snap.alloc_count, static_cast<uint64_t>(kAllocsPerThread));
    }
}
