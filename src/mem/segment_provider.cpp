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

#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mem/segment_provider.hpp>
#include <hpactor/mem/super_carrier.hpp>
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
    void* slab = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slab = carve_from_segment(sc);
    }
    if (slab)
        return slab;

    // MEM-004: Try super carrier before individual mmap
    // (carrier carve is lock-free via atomic offset — no mutex needed)
    if (super_carrier_ && super_carrier_->is_initialized()) {
        slab = super_carrier_->carve(slab_size(sc));
        if (slab)
            return slab;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return allocate_new_segment(slab_size(sc));
}

void SegmentProvider::release_slab(void* slab, SizeClass /*sc*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = slab_records_.find(slab);
    if (it == slab_records_.end()) {
        return;
    }

    uint32_t idx = it->second.segment_index;
    slab_records_.erase(it);

    if (idx >= segments_.size()) {
        return;
    }

    if (segments_[idx].dec_ref() == 0) {
        // Last slab — munmap the whole segment
        munmap(segments_[idx].base, segments_[idx].size);
        segments_.erase(segments_.begin() + idx);
        // Update indices for slabs in segments that shifted
        for (auto& [ptr, rec] : slab_records_) {
            if (rec.segment_index > idx) {
                --rec.segment_index;
            }
        }
    }
}

SegmentProvider::SegmentInfo SegmentProvider::lookup(void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& [slab_base, rec] : slab_records_) {
        if (ptr >= slab_base &&
            ptr < static_cast<std::byte*>(slab_base) + rec.slab_size_bytes) {
            if (rec.segment_index < segments_.size()) {
                return {segments_[rec.segment_index].base,
                        segments_[rec.segment_index].size};
            }
            return {nullptr, 0};
        }
    }

    // Fallback linear scan over segments
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
    s.huge_page_segments = huge_page_count_;
    s.thp_segments = thp_count_;
    s.regular_segments = regular_count_;
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
            SlabRecord rec;
            rec.segment_index = static_cast<uint32_t>(i);
            rec.slab_size_bytes = needed;
            slab_records_[slab] = rec;
            return slab;
        }
    }
    return nullptr;
}

void SegmentProvider::register_slab_owner(void* slab, size_t slab_size,
                                          void* owner_cache, RegionType region,
                                          SizeClass sc) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = slab_records_.find(slab);
    if (it != slab_records_.end()) {
        it->second.owner_cache = owner_cache;
        it->second.region = region;
        it->second.size_class = sc;
        it->second.slab_size_bytes = slab_size;
    }
}

SegmentProvider::SlabInfo SegmentProvider::lookup_slab(void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& [slab_base, rec] : slab_records_) {
        auto* begin = static_cast<std::byte*>(slab_base);
        auto* end = begin + rec.slab_size_bytes;
        if (ptr >= begin && ptr < end && rec.segment_index < segments_.size()) {
            return SlabInfo{segments_[rec.segment_index].base,
                            slab_base,
                            segments_[rec.segment_index].size,
                            rec.slab_size_bytes,
                            rec.owner_cache,
                            rec.region,
                            rec.size_class,
                            true};
        }
    }
    return SlabInfo{};
}

void* SegmentProvider::allocate_new_segment(size_t size) {
    size_t alloc_size = (size > kSegmentSize) ? size : kSegmentSize;

    FAULT_INJECT("hpactor.allocator.segment.mmap_fail") {
        return nullptr;
    }

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    bool used_huge = false;
    bool used_thp = false;

    // MEM-005: Try huge pages for legacy segments
#ifdef MAP_HUGETLB
    if (huge_info_.explicit_huge_pages_available) {
        void* probe = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                           flags | MAP_HUGETLB, -1, 0);
        if (probe != MAP_FAILED) {
            huge_page_count_++;
            Segment seg;
            seg.base = probe;
            seg.size = alloc_size;
            seg.offset = size;
            seg.ref_count = 1;
            segments_.push_back(seg);
            uint32_t idx = static_cast<uint32_t>(segments_.size() - 1);
            SlabRecord rec;
            rec.segment_index = idx;
            rec.slab_size_bytes = size;
            slab_records_[probe] = rec;
            return probe;
        }
        // Fall through to standard pages with THP hint
    }
#endif

    void* base = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, flags, -1, 0);

    if (base == MAP_FAILED) {
        HPACTOR_LOG_WARNING(log::LogCategory::kMemory, ActorId{0},
                            static_cast<uint32_t>(log::LogEventId::kMemoryAlloc),
                            "segment allocation failed (mmap)",
                            log::field("size", static_cast<uint64_t>(alloc_size)));
        return nullptr;
    }

#ifdef MADV_HUGEPAGE
    if (huge_info_.transparent_huge_pages_available) {
        madvise(base, alloc_size, MADV_HUGEPAGE);
        used_thp = true;
    }
#endif

    if (used_huge)
        huge_page_count_++;
    else if (used_thp)
        thp_count_++;
    else
        regular_count_++;

    Segment seg;
    seg.base = base;
    seg.size = alloc_size;
    seg.offset = size;
    seg.ref_count = 1;

    segments_.push_back(seg);
    uint32_t idx = static_cast<uint32_t>(segments_.size() - 1);
    SlabRecord rec;
    rec.segment_index = idx;
    rec.slab_size_bytes = size;
    slab_records_[base] = rec;

    return base;
}

} // namespace hpactor::mem
