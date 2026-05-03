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

#include <hpactor/mem/segment_provider.hpp>
#include <hpactor/mem/size_class.hpp>

#include <cassert>
#include <cstring>
#include <iostream>
#include <set>

int main() {
    using namespace hpactor::mem;

    SegmentProvider& sp = SegmentProvider::instance();

    // Acquire slabs of different size classes
    void* slab32  = sp.acquire_slab(SizeClass::k32B);
    void* slab64  = sp.acquire_slab(SizeClass::k64B);
    void* slab128 = sp.acquire_slab(SizeClass::k128B);
    void* slab4k  = sp.acquire_slab(SizeClass::k4KB);

    assert(slab32 != nullptr);
    assert(slab64 != nullptr);
    assert(slab128 != nullptr);
    assert(slab4k != nullptr);

    // All pointers should be distinct
    std::set<void*> ptrs = {slab32, slab64, slab128, slab4k};
    assert(ptrs.size() == 4);

    // Each slab should be writable
    size_t sz32 = sp.slab_size(SizeClass::k32B);
    size_t sz4k = sp.slab_size(SizeClass::k4KB);
    assert(sz32 > 0);
    assert(sz4k > 0);
    assert(sz32 < sz4k);

    std::memset(slab32, 0xAB, sz32);
    std::memset(slab4k, 0xCD, sz4k);

    // Verify slab size ratios
    assert(sp.slab_size(SizeClass::k256B) == sz32 * 2);   // 128KB
    assert(sp.slab_size(SizeClass::k512B) == sz32 * 4);   // 256KB
    assert(sp.slab_size(SizeClass::k4KB)  == sz32 * 8);   // 512KB

    // Verify address-to-segment lookup
    SegmentProvider::SegmentInfo info32 = sp.lookup(slab32);
    assert(info32.base != nullptr);
    assert(info32.size >= sz32);

    // Verify stats
    auto s = sp.stats();
    assert(s.active_segments > 0);
    assert(s.total_allocated > 0);

    // Release slabs back
    sp.release_slab(slab32, SizeClass::k32B);
    sp.release_slab(slab64, SizeClass::k64B);
    sp.release_slab(slab128, SizeClass::k128B);
    sp.release_slab(slab4k, SizeClass::k4KB);

    std::cout << "test_segment_provider: PASS\n";
    return 0;
}
