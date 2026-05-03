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
#include <hpactor/platform.hpp>

#include <algorithm>
#include <cstring>
#include <sys/mman.h>

namespace hpactor::mem {

SegmentProvider& SegmentProvider::instance() {
    static SegmentProvider sp;
    return sp;
}

void* SegmentProvider::acquire_slab(SizeClass sc) {
    std::lock_guard<std::mutex> lock(mutex_);

    void* slab = carve_from_segment(sc);
    if (slab) {
        return slab;
    }

    return allocate_new_segment(slab_size(sc));
}

void SegmentProvider::release_slab(void* slab, SizeClass /*sc*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = addr_to_segment_.find(slab);
    if (it == addr_to_segment_.end()) {
        return;
    }

    Segment* seg = it->second;
    addr_to_segment_.erase(it);

    if (seg->dec_ref() == 0) {
        // Last slab — munmap the whole segment
        munmap(seg->base, seg->size);
        auto seg_it = std::find_if(segments_.begin(), segments_.end(),
                                   [seg](const Segment& s) { return &s == seg; });
        if (seg_it != segments_.end()) {
            segments_.erase(seg_it);
        }
    }
}

SegmentProvider::SegmentInfo SegmentProvider::lookup(void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = addr_to_segment_.find(ptr);
    if (it != addr_to_segment_.end()) {
        return {it->second->base, it->second->size};
    }

    // Linear scan for interior pointers
    for (const auto& seg : segments_) {
        if (ptr >= seg.base
            && ptr < static_cast<std::byte*>(seg.base) + seg.size) {
            return {seg.base, seg.size};
        }
    }
    return {nullptr, 0};
}

size_t SegmentProvider::slab_size(SizeClass sc) const {
    // Multiplier per size class (base = 64KB)
    static constexpr uint8_t kMultiplier[kNumSizeClasses] = {
        1, 1, 1,   // 32B, 64B, 128B → 64KB
        2,          // 256B → 128KB
        4, 4,       // 512B, 1KB → 256KB
        8, 8        // 2KB, 4KB → 512KB
    };
    return kBaseSlabSize * kMultiplier[static_cast<uint8_t>(sc)];
}

SegmentProvider::Stats SegmentProvider::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s;
    s.active_segments = segments_.size();
    for (const auto& seg : segments_) {
        s.total_allocated += seg.size;
    }
    return s;
}

void* SegmentProvider::carve_from_segment(SizeClass sc) {
    size_t needed = slab_size(sc);
    for (auto& seg : segments_) {
        if (seg.size - seg.offset >= needed) {
            void* slab = static_cast<std::byte*>(seg.base) + seg.offset;
            seg.offset += needed;
            seg.inc_ref();
            addr_to_segment_[slab] = &seg;
            return slab;
        }
    }
    return nullptr;
}

void* SegmentProvider::allocate_new_segment(size_t size) {
    size_t alloc_size = (size > kSegmentSize) ? size : kSegmentSize;

    void* base = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (base == MAP_FAILED) {
        return nullptr;
    }

    Segment seg;
    seg.base = base;
    seg.size = alloc_size;
    seg.offset = size; // first `size` bytes are the returned slab
    seg.ref_count = 1;

    segments_.push_back(seg);
    Segment* seg_ptr = &segments_.back();
    addr_to_segment_[base] = seg_ptr;

    return base;
}

} // namespace hpactor::mem
