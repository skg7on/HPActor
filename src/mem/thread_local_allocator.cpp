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

#include <hpactor/mem/thread_local_allocator.hpp>

namespace hpactor::mem {

ThreadLocalAllocator::ThreadLocalAllocator() {
    for (uint8_t r = 0; r < kNumRegionTypes; ++r) {
        for (uint8_t s = 0; s < kNumSizeClasses; ++s) {
            caches_[r][s] = new SlabCache(static_cast<SizeClass>(s),
                                          static_cast<RegionType>(r));
        }
    }
}

ThreadLocalAllocator::ThreadLocalAllocator(const MemoryStrategyTable& table) {
    for (uint8_t r = 0; r < kNumRegionTypes; ++r) {
        const auto& cfg = table.regions[r];
        for (uint8_t s = 0; s < kNumSizeClasses; ++s) {
            caches_[r][s] = new SlabCache(static_cast<SizeClass>(s),
                                          static_cast<RegionType>(r),
                                          cfg.strategy, cfg.enable_coalescing);
        }
    }
}

ThreadLocalAllocator::~ThreadLocalAllocator() {
    for (uint8_t r = 0; r < kNumRegionTypes; ++r) {
        for (uint8_t s = 0; s < kNumSizeClasses; ++s) {
            delete caches_[r][s];
        }
    }
}

void* ThreadLocalAllocator::allocate(RegionType region, SizeClass sc,
                                     ActorId owner) noexcept {
    return caches_[static_cast<uint8_t>(region)][static_cast<uint8_t>(sc)]->allocate(
        owner);
}

void* ThreadLocalAllocator::allocate(RegionType region, size_t user_bytes,
                                     ActorId owner) noexcept {
    SizeClass sc = class_for_size(user_bytes);
    return allocate(region, sc, owner);
}

void ThreadLocalAllocator::deallocate(void* user_ptr) noexcept {
    auto* hdr = AllocHeader::from_user_data(user_ptr);
    auto slab = SegmentProvider::instance().lookup_slab(hdr);
    if (slab.found && slab.owner_cache) {
        static_cast<SlabCache*>(slab.owner_cache)->deallocate(user_ptr);
        return;
    }
    const auto region = hdr->region();
    const auto sc = static_cast<SizeClass>(hdr->size_class);
    caches_[static_cast<uint8_t>(region)][static_cast<uint8_t>(sc)]->deallocate(
        user_ptr);
}

const SlabCache::Stats& ThreadLocalAllocator::stats(SizeClass sc) const noexcept {
    return caches_[static_cast<uint8_t>(RegionType::kInternal)][static_cast<uint8_t>(sc)]
        ->stats();
}

const SlabCache::Stats&
ThreadLocalAllocator::stats(RegionType region, SizeClass sc) const noexcept {
    return caches_[static_cast<uint8_t>(region)][static_cast<uint8_t>(sc)]->stats();
}

} // namespace hpactor::mem
