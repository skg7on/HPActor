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

#pragma once

#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/slab_cache.hpp>

#include <array>
#include <cstddef>

namespace hpactor::mem {

// Per-thread allocator. Owns a SlabCache for each (RegionType × SizeClass)
// combination. The hot allocation path is bump-pointer or freelist pop — no
// locks.
class ThreadLocalAllocator {
  public:
    ThreadLocalAllocator();
    ~ThreadLocalAllocator();

    ThreadLocalAllocator(const ThreadLocalAllocator&) = delete;
    ThreadLocalAllocator& operator=(const ThreadLocalAllocator&) = delete;
    ThreadLocalAllocator(ThreadLocalAllocator&&) = delete;
    ThreadLocalAllocator& operator=(ThreadLocalAllocator&&) = delete;

    // Region-aware allocation from a specific size class.
    void* allocate(RegionType region, SizeClass sc, ActorId owner) noexcept;

    // Region-aware allocation by user-requested byte size.
    void* allocate(RegionType region, size_t user_bytes, ActorId owner) noexcept;

    // Backward-compatible: allocate with default region (kInternal).
    void* allocate(SizeClass sc, ActorId owner) noexcept {
        return allocate(RegionType::kInternal, sc, owner);
    }

    // Backward-compatible: allocate by bytes with default region.
    void* allocate_bytes(size_t user_bytes, ActorId owner) noexcept {
        return allocate(RegionType::kInternal, user_bytes, owner);
    }

    // Deallocate a block. Routes to origin SlabCache for cross-thread frees.
    void deallocate(void* user_ptr) noexcept;

    // Stats for a specific size class (default region).
    const SlabCache::Stats& stats(SizeClass sc) const noexcept;

    // Stats for a specific region and size class.
    const SlabCache::Stats& stats(RegionType region, SizeClass sc) const noexcept;

  private:
    using CacheRow = std::array<SlabCache*, kNumSizeClasses>;
    std::array<CacheRow, kNumRegionTypes> caches_{};
};

} // namespace hpactor::mem
