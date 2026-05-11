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

#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
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

    uint32_t idx = it->second;
    addr_to_segment_.erase(it);

    if (idx >= segments_.size()) {
        return;
    }

    if (segments_[idx].dec_ref() == 0) {
        // Last slab — munmap the whole segment
        munmap(segments_[idx].base, segments_[idx].size);
        segments_.erase(segments_.begin() + idx);
        // Update indices for slabs in segments that shifted
        for (auto& [ptr, i] : addr_to_segment_) {
            if (i > idx) {
                --i;
            }
        }
    }
}

SegmentProvider::SegmentInfo SegmentProvider::lookup(void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = addr_to_segment_.find(ptr);
    if (it != addr_to_segment_.end()) {
        auto idx = it->second;
        if (idx < segments_.size()) {
            return {segments_[idx].base, segments_[idx].size};
        }
        return {nullptr, 0};
    }

    // Linear scan for interior pointers
    for (const auto& seg : segments_) {
        if (ptr >= seg.base && ptr < static_cast<std::byte*>(seg.base) + seg.size) {
            return {seg.base, seg.size};
        }
    }
    return {nullptr, 0};
}

size_t SegmentProvider::slab_size(SizeClass sc) const {
    // Multiplier per size class (base = 64KB)
    static constexpr uint8_t kMultiplier[kNumSizeClasses] = {
        1, 1, 1, // 32B, 64B, 128B → 64KB
        2,       // 256B → 128KB
        4, 4,    // 512B, 1KB → 256KB
        8, 8     // 2KB, 4KB → 512KB
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
    for (size_t i = 0; i < segments_.size(); ++i) {
        auto& seg = segments_[i];
        if (seg.size - seg.offset >= needed) {
            void* slab = static_cast<std::byte*>(seg.base) + seg.offset;
            seg.offset += needed;
            seg.inc_ref();
            addr_to_segment_[slab] = static_cast<uint32_t>(i);
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
        HPACTOR_LOG_WARNING(log::LogCategory::kMemory, ActorId{0},
                            static_cast<uint32_t>(log::LogEventId::kMemoryAlloc),
                            "segment allocation failed (mmap)",
                            log::field("size", static_cast<uint64_t>(alloc_size)));
        return nullptr;
    }

    Segment seg;
    seg.base = base;
    seg.size = alloc_size;
    seg.offset = size; // first `size` bytes are the returned slab
    seg.ref_count = 1;

    segments_.push_back(seg);
    uint32_t idx = static_cast<uint32_t>(segments_.size() - 1);
    addr_to_segment_[base] = idx;

    return base;
}

} // namespace hpactor::mem
