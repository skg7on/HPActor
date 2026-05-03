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

#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    auto& mt = hpactor::mem::MemoryTracker::instance();
    hpactor::mem::ActorMemoryStats snap;

    // Record allocations for several actors
    assert(mt.record_alloc(hpactor::ActorId{1}, 128));
    assert(mt.record_alloc(hpactor::ActorId{1}, 256));
    assert(mt.record_alloc(hpactor::ActorId{2}, 512));

    // Check per-actor snapshots
    mt.snapshot(hpactor::ActorId{1}, snap);
    assert(snap.current_bytes == 384); // 128 + 256
    assert(snap.peak_bytes == 384);
    assert(snap.alloc_count == 2);

    mt.snapshot(hpactor::ActorId{2}, snap);
    assert(snap.current_bytes == 512);
    assert(snap.alloc_count == 1);

    // Record frees
    mt.record_free(hpactor::ActorId{1}, 128);
    mt.snapshot(hpactor::ActorId{1}, snap);
    assert(snap.current_bytes == 256); // 384 - 128
    assert(snap.free_count == 1);
    assert(snap.peak_bytes == 384); // peak unchanged

    // Global aggregates
    assert(mt.total_active_bytes() == 768); // 256 + 512
    assert(mt.total_alloc_count() == 3);

    // Concurrent tracking from multiple threads
    {
        constexpr int kThreads = 4;
        constexpr int kAllocsPerThread = 1000;

        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&mt, t]() {
                for (int i = 0; i < kAllocsPerThread; ++i) {
                    mt.record_alloc(
                        hpactor::ActorId{static_cast<uint64_t>(t + 100)},
                        static_cast<size_t>(64));
                }
            });
        }
        for (auto& th : threads) th.join();

        // Verify no data corruption from concurrent access
        for (int t = 0; t < kThreads; ++t) {
            mt.snapshot(hpactor::ActorId{static_cast<uint64_t>(t + 100)}, snap);
            assert(snap.alloc_count == kAllocsPerThread);
        }
    }

    std::cout << "test_memory_tracker: PASS\n";
    return 0;
}
