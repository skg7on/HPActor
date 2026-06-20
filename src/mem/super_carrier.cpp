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

// Required for MAP_HUGETLB, MADV_HUGEPAGE, MAP_NORESERVE, getcpu on Linux with
// -std=c++20 (CMAKE_CXX_EXTENSIONS OFF suppresses the implicit _GNU_SOURCE).
#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

#include <hpactor/mem/super_carrier.hpp>

#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)
#    include <sys/syscall.h>
#endif

namespace hpactor::mem {

HugePageInfo probe_huge_pages() noexcept {
    HugePageInfo info;
#if defined(__LP64__) && defined(__linux__)
    void* probe = mmap(nullptr, 2 * 1024 * 1024, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (probe != MAP_FAILED) {
        info.explicit_huge_pages_available = true;
        info.huge_page_size = 2 * 1024 * 1024;
        munmap(probe, 2 * 1024 * 1024);
    }

    // Probe 1GB pages
#    ifdef MAP_HUGE_1GB
    probe = mmap(nullptr, 1024ULL * 1024 * 1024, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB, -1, 0);
    if (probe != MAP_FAILED) {
        info.huge_page_size_1gb = 1024ULL * 1024 * 1024;
        munmap(probe, 1024ULL * 1024 * 1024);
    }
#    endif

    FILE* f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    if (f) {
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), f)) {
            info.transparent_huge_pages_available = (buf[1] == 'a');
        }
        fclose(f);
    }
#endif
    return info;
}

// ── MEM-007: NUMA topology detection ────────────────────────────

NumaInfo probe_numa_topology() noexcept {
    NumaInfo info;
#if defined(__linux__)
    // Determine max node by probing with getcpu on each possible CPU.
    // A simpler approach: read /sys/devices/system/node/online.
    FILE* f = fopen("/sys/devices/system/node/online", "r");
    if (f) {
        // Format: "0" or "0-3" or "0,2,4"
        unsigned max_node = 0;
        unsigned start = 0;
        unsigned end = 0;
        int n = 0;
        // parse "0-3\n" → max_node = 3, node_count = 4
        if (fscanf(f, "%u-%u%n", &start, &end, &n) >= 2 && n > 0) {
            max_node = end;
        } else {
            rewind(f);
            // single node: "0\n"
            if (fscanf(f, "%u", &max_node) >= 1) {
                // max_node already set
            }
        }
        fclose(f);
        info.node_count = max_node + 1;
        if (info.node_count < 1)
            info.node_count = 1;
        if (info.node_count > SuperCarrier::kMaxNumaNodes)
            info.node_count = SuperCarrier::kMaxNumaNodes;
    }
#endif
    return info;
}

unsigned get_current_numa_node() noexcept {
#if defined(__linux__) && defined(SYS_getcpu)
    unsigned cpu = 0;
    unsigned node = 0;
    if (syscall(SYS_getcpu, &cpu, &node, nullptr) == 0) {
        return node;
    }
#else
    (void)0;
#endif
    return 0;
}

// ── SuperCarrier ────────────────────────────────────────────────

bool SuperCarrier::init(size_t size_bytes, const HugePageInfo& huge_info,
                        const NumaInfo& numa_info) noexcept {
    (void)huge_info;
    (void)numa_info;
#ifdef __LP64__
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#    ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#    endif

    bool ok = false;
#    ifdef MAP_HUGETLB
    if (huge_info.explicit_huge_pages_available) {
        void* addr =
            mmap(nullptr, size_bytes, PROT_NONE, flags | MAP_HUGETLB, -1, 0);
        if (addr != MAP_FAILED) {
            carrier_base_ = addr;
            carrier_size_ = size_bytes;
            ok = true;
        }
    }
#    endif
    if (!ok) {
        void* addr = mmap(nullptr, size_bytes, PROT_NONE, flags, -1, 0);
        if (addr == MAP_FAILED)
            return false;

#    ifdef MADV_HUGEPAGE
        if (huge_info.transparent_huge_pages_available)
            madvise(addr, size_bytes, MADV_HUGEPAGE);
#    endif
        carrier_base_ = addr;
        carrier_size_ = size_bytes;
    }

    carve_offset_.store(0, std::memory_order_relaxed);
    released_bytes_.store(0, std::memory_order_relaxed);

    // MEM-007: Partition the carrier for NUMA
    numa_node_count_ = numa_info.node_count;
    if (numa_node_count_ > 1) {
        numa_node_size_ = carrier_size_ / numa_node_count_;
        for (uint32_t n = 0; n < numa_node_count_; ++n) {
            numa_carve_offsets_[n].store(0, std::memory_order_relaxed);
        }
        carve_offset_.store(numa_node_size_ * numa_node_count_,
                            std::memory_order_relaxed);
    }
    return true;
#else
    (void)size_bytes;
    return false;
#endif
}

void* SuperCarrier::carve_from_offset(std::atomic<size_t>& offset,
                                      size_t region_base, size_t region_end,
                                      size_t slab_size_bytes) noexcept {
#ifdef __LP64__
    size_t off = offset.fetch_add(slab_size_bytes, std::memory_order_relaxed);
    if (region_base + off + slab_size_bytes > region_end) {
        offset.fetch_sub(slab_size_bytes, std::memory_order_relaxed);
        return nullptr;
    }
    void* addr = static_cast<char*>(carrier_base_) + region_base + off;
    if (mprotect(addr, slab_size_bytes, PROT_READ | PROT_WRITE) != 0) {
        offset.fetch_sub(slab_size_bytes, std::memory_order_relaxed);
        return nullptr;
    }
    return addr;
#else
    (void)offset;
    (void)region_base;
    (void)region_end;
    (void)slab_size_bytes;
    return nullptr;
#endif
}

void* SuperCarrier::carve(size_t slab_size_bytes) noexcept {
    return carve_from_offset(carve_offset_, 0, carrier_size_, slab_size_bytes);
}

void* SuperCarrier::carve_numa(size_t slab_size_bytes, unsigned numa_node) noexcept {
#ifdef __LP64__
    if (!carrier_base_)
        return nullptr;
    // Try the requested NUMA node first (MEM-007)
    if (numa_node_count_ > 1 && numa_node < numa_node_count_) {
        size_t region_start = static_cast<size_t>(numa_node) * numa_node_size_;
        size_t region_end = region_start + numa_node_size_;
        void* addr = carve_from_offset(numa_carve_offsets_[numa_node],
                                       region_start, region_end, slab_size_bytes);
        if (addr)
            return addr;
    }
    // Fall back to global carve (node-agnostic or exhausted local node)
    return carve(slab_size_bytes);
#else
    (void)slab_size_bytes;
    (void)numa_node;
    return nullptr;
#endif
}

bool SuperCarrier::grow(size_t additional_bytes) noexcept {
    (void)additional_bytes;
#ifdef __LP64__
#    if defined(__linux__) && defined(MREMAP_MAYMOVE)
    if (!can_grow_ || !carrier_base_)
        return false;
    size_t new_size = carrier_size_ + additional_bytes;
    if (max_carrier_size_ > 0 && new_size > max_carrier_size_)
        return false;
    void* new_base = mremap(carrier_base_, carrier_size_, new_size, MREMAP_MAYMOVE);
    if (new_base == MAP_FAILED)
        return false;
    carrier_base_ = new_base;
    carrier_size_ = new_size;
    return true;
#    endif
#endif
    return false;
}

void SuperCarrier::release(void* slab_addr, size_t slab_size_bytes) noexcept {
#ifdef __LP64__
    if (!carrier_base_)
        return;
    if (slab_addr < carrier_base_ ||
        slab_addr >= static_cast<char*>(carrier_base_) + carrier_size_)
        return;
    madvise(slab_addr, slab_size_bytes, MADV_FREE);
    mprotect(slab_addr, slab_size_bytes, PROT_NONE);
    released_bytes_.fetch_add(slab_size_bytes, std::memory_order_relaxed);
#else
    (void)slab_addr;
    (void)slab_size_bytes;
#endif
}

} // namespace hpactor::mem
