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

// Required for MAP_HUGETLB, MADV_HUGEPAGE, MAP_NORESERVE on Linux with
// -std=c++20 (CMAKE_CXX_EXTENSIONS OFF suppresses the implicit _GNU_SOURCE).
#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

#include <hpactor/mem/super_carrier.hpp>

#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

namespace hpactor::mem {

HugePageInfo probe_huge_pages() noexcept {
    HugePageInfo info;
#if defined(__LP64__) && defined(__linux__)
    // Probe MAP_HUGETLB (Linux-only)
    void* probe = mmap(nullptr, 2 * 1024 * 1024, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (probe != MAP_FAILED) {
        info.explicit_huge_pages_available = true;
        info.huge_page_size = 2 * 1024 * 1024; // 2MB
        munmap(probe, 2 * 1024 * 1024);
    }

    // Check THP availability
    FILE* f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    if (f) {
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), f)) {
            // "[always] madvise never" → THP is available
            info.transparent_huge_pages_available = (buf[1] == 'a');
        }
        fclose(f);
    }
#endif
    return info;
}

bool SuperCarrier::init(size_t size_bytes, const HugePageInfo& huge_info) noexcept {
    (void)huge_info;
#ifdef __LP64__
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#    ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#    endif

    // MEM-005: Use huge pages when available (fallback hierarchy)
#    if defined(__linux__)
    if (huge_info.explicit_huge_pages_available) {
        void* addr =
            mmap(nullptr, size_bytes, PROT_NONE, flags | MAP_HUGETLB, -1, 0);
        if (addr != MAP_FAILED) {
            carrier_base_ = addr;
            carrier_size_ = size_bytes;
            carve_offset_.store(0, std::memory_order_relaxed);
            released_bytes_.store(0, std::memory_order_relaxed);
            return true;
        }
        // Fall through to standard pages
    }
#    endif

    void* addr = mmap(nullptr, size_bytes, PROT_NONE, flags, -1, 0);
    if (addr == MAP_FAILED) {
        return false;
    }

    // Use THP hint on standard-page allocation
#    if defined(__linux__)
    if (huge_info.transparent_huge_pages_available) {
        madvise(addr, size_bytes, MADV_HUGEPAGE);
    }
#    endif

    carrier_base_ = addr;
    carrier_size_ = size_bytes;
    carve_offset_.store(0, std::memory_order_relaxed);
    released_bytes_.store(0, std::memory_order_relaxed);
    return true;
#else
    (void)size_bytes;
    (void)huge_info;
    return false;
#endif
}

void* SuperCarrier::carve(size_t slab_size_bytes) noexcept {
#ifdef __LP64__
    if (!carrier_base_)
        return nullptr;

    size_t offset =
        carve_offset_.fetch_add(slab_size_bytes, std::memory_order_relaxed);
    if (offset + slab_size_bytes > carrier_size_) {
        carve_offset_.fetch_sub(slab_size_bytes, std::memory_order_relaxed);
        return nullptr;
    }
    void* addr = static_cast<char*>(carrier_base_) + offset;
    if (mprotect(addr, slab_size_bytes, PROT_READ | PROT_WRITE) != 0) {
        carve_offset_.fetch_sub(slab_size_bytes, std::memory_order_relaxed);
        return nullptr;
    }
    return addr;
#else
    (void)slab_size_bytes;
    return nullptr;
#endif
}

void SuperCarrier::release(void* slab_addr, size_t slab_size_bytes) noexcept {
#ifdef __LP64__
    if (!carrier_base_)
        return;
    if (slab_addr < carrier_base_ ||
        slab_addr >= static_cast<char*>(carrier_base_) + carrier_size_) {
        return;
    }
    madvise(slab_addr, slab_size_bytes, MADV_FREE);
    mprotect(slab_addr, slab_size_bytes, PROT_NONE);
    released_bytes_.fetch_add(slab_size_bytes, std::memory_order_relaxed);
#else
    (void)slab_addr;
    (void)slab_size_bytes;
#endif
}

} // namespace hpactor::mem
