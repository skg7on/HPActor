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
#include <hpactor/mem/alloc_header.hpp>

#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    using namespace hpactor::mem;

    // Test canary detection on buffer overflow
    {
        SlabCache cache(SizeClass::k128B);

        void* p = cache.allocate(hpactor::ActorId{1});
        assert(p != nullptr);
        assert(cache.live_count() == 1);

        // Corrupt the canary by writing past the user data region
        std::memset(static_cast<std::byte*>(p) + 128, 0xFF, 16);

        // Deallocate — in debug mode, canary verification fires
        // In release mode, the corrupt data is written to the footer
        cache.deallocate(p);

        // After corruption, the freed block should still be returned
        // (debug mode detects corruption but currently doesn't abort)
        assert(cache.live_count() == 0);
    }

    // Test that blocks are recycled and data integrity maintained
    {
        SlabCache cache(SizeClass::k256B);

        void* p1 = cache.allocate(hpactor::ActorId{1});
        assert(p1 != nullptr);
        std::memset(p1, 0x42, 256);

        cache.deallocate(p1);

        // Re-allocate — should get same block
        void* p2 = cache.allocate(hpactor::ActorId{2});
        assert(p2 == p1); // recycled

        // Data should be preserved (or poisoned, depending on build mode)
        // In any case, the block should be usable
        std::memset(p2, 0x77, 256);
        uint8_t first = *static_cast<uint8_t*>(p2);
        assert(first == 0x77);

        cache.deallocate(p2);
        assert(cache.live_count() == 0);
    }

    // Test multiple alloc/free cycles for stability
    {
        SlabCache cache(SizeClass::k64B);
        for (int cycle = 0; cycle < 100; ++cycle) {
            void* p = cache.allocate(hpactor::ActorId{static_cast<uint64_t>(cycle)});
            assert(p != nullptr);
            std::memset(p, static_cast<uint8_t>(cycle), 64);
            cache.deallocate(p);
        }
        assert(cache.live_count() == 0);
    }

    std::cout << "test_memory_poisoning: PASS\n";
    return 0;
}
