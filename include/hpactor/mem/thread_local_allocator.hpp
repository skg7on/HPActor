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

#include <hpactor/mem/slab_cache.hpp>
#include <hpactor/mem/size_class.hpp>

#include <array>
#include <cstddef>

namespace hpactor::mem {

// Per-thread allocator. Owns a SlabCache for each size class.
// The hot allocation path is bump-pointer or freelist pop — no locks.
class ThreadLocalAllocator {
  public:
    ThreadLocalAllocator();
    ~ThreadLocalAllocator();

    ThreadLocalAllocator(const ThreadLocalAllocator&) = delete;
    ThreadLocalAllocator& operator=(const ThreadLocalAllocator&) = delete;
    ThreadLocalAllocator(ThreadLocalAllocator&&) = delete;
    ThreadLocalAllocator& operator=(ThreadLocalAllocator&&) = delete;

    // Allocate from a specific size class. Returns user-data pointer.
    void* allocate(SizeClass sc, ActorId owner) noexcept;

    // Allocate by user-requested byte size (auto-selects size class).
    void* allocate_bytes(size_t user_bytes, ActorId owner) noexcept;

    // Deallocate a block.
    void deallocate(void* user_ptr) noexcept;

    // Stats for a specific size class.
    const SlabCache::Stats& stats(SizeClass sc) const noexcept;

  private:
    std::array<SlabCache*, kNumSizeClasses> caches_;
};

} // namespace hpactor::mem
