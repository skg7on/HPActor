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

#include <hpactor/mem/slab_cache.hpp>
#include <hpactor/mem/size_class.hpp>

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

int main() {
    using namespace hpactor::mem;

    // Create a slab cache for 128B size class
    SlabCache cache(SizeClass::k128B);

    assert(cache.size_class() == SizeClass::k128B);
    assert(cache.live_count() == 0);

    // Allocate several blocks
    void* b1 = cache.allocate(hpactor::ActorId{1});
    void* b2 = cache.allocate(hpactor::ActorId{2});
    void* b3 = cache.allocate(hpactor::ActorId{3});

    assert(b1 != nullptr);
    assert(b2 != nullptr);
    assert(b3 != nullptr);
    assert(b1 != b2);
    assert(b2 != b3);
    assert(cache.live_count() == 3);

    // Each block should be independently writeable
    std::memset(b1, 0x11, 128);
    std::memset(b2, 0x22, 128);
    std::memset(b3, 0x33, 128);
    assert(*static_cast<uint8_t*>(b1) == 0x11);
    assert(*static_cast<uint8_t*>(b2) == 0x22);
    assert(*static_cast<uint8_t*>(b3) == 0x33);

    // Free one block and reallocate — should recycle
    cache.deallocate(b1);
    assert(cache.live_count() == 2);

    void* b4 = cache.allocate(hpactor::ActorId{4});
    assert(b4 != nullptr);
    assert(b4 == b1); // recycled from freelist
    assert(cache.live_count() == 3);

    // Free all
    cache.deallocate(b2);
    cache.deallocate(b3);
    cache.deallocate(b4);
    assert(cache.live_count() == 0);

    // Allocate many blocks to force multiple slab acquisitions
    std::vector<void*> blocks;
    for (int i = 0; i < 1000; ++i) {
        void* b = cache.allocate(hpactor::ActorId{static_cast<uint64_t>(i)});
        assert(b != nullptr);
        std::memset(b, static_cast<uint8_t>(i & 0xFF), 128);
        blocks.push_back(b);
    }
    assert(cache.live_count() == 1000);

    // Free all
    for (auto* b : blocks) {
        cache.deallocate(b);
    }
    assert(cache.live_count() == 0);

    // Verify stats
    auto& stats = cache.stats();
    assert(stats.alloc_count.load() == 1004); // 3 + 1 (b4) + 1000
    assert(stats.free_count.load() == 1004);

    std::cout << "test_slab_cache: PASS\n";
    return 0;
}
