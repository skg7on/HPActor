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

#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/freelist.hpp>
#include <hpactor/mem/segment_provider.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hpactor::mem {

// Per-size-class slab cache. Manages one or more slabs of the same size class.
// Uses bump allocation for virgin memory + lock-free freelist for recycled blocks.
class SlabCache {
  public:
    struct Stats {
        std::atomic<uint64_t> alloc_count{0};
        std::atomic<uint64_t> free_count{0};
        std::atomic<uint64_t> slab_acquire_count{0};
    };

    explicit SlabCache(SizeClass sc) : size_class_(sc) {}

    ~SlabCache();

    SlabCache(const SlabCache&) = delete;
    SlabCache& operator=(const SlabCache&) = delete;
    SlabCache(SlabCache&&) = delete;
    SlabCache& operator=(SlabCache&&) = delete;

    // Allocate a block from this cache. Returns pointer to user data.
    void* allocate(ActorId owner) noexcept;

    // Free a block back to this cache.
    void deallocate(void* user_ptr) noexcept;

    SizeClass size_class() const noexcept { return size_class_; }
    uint32_t live_count() const noexcept { return live_count_.load(); }
    const Stats& stats() const noexcept { return stats_; }

  private:
    void refill();

    SizeClass size_class_;
    uint8_t current_generation_{0};

    std::byte* current_slab_{nullptr};
    size_t slab_size_{0};
    size_t bump_offset_{0};

    FreeList<AllocHeader> freelist_;
    std::atomic<uint32_t> live_count_{0};
    Stats stats_;

    std::vector<std::byte*> slabs_;
};

} // namespace hpactor::mem
