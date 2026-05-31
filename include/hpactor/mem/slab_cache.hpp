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

#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/freelist.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/segment_provider.hpp>
#include <hpactor/mem/size_class.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hpactor::mem {

/// \brief Per-size-class slab cache.
///
/// Manages one or more slabs of the same size class. Uses bump allocation for
/// virgin memory and a lock-free freelist for recycled blocks. Owned by
/// ThreadLocalAllocator — one cache per (RegionType, SizeClass) pair per
/// thread.
///
/// \note Not thread-safe on its own. Each cache is confined to a single thread.
///       Cross-thread frees are routed to the origin cache via
///       SegmentProvider::lookup_slab().
class SlabCache {
  public:
    /// \brief Per-cache allocation and deallocation counters.
    struct Stats {
        std::atomic<uint64_t> alloc_count{0}; ///< Cumulative allocations from
                                              ///< this cache.
        std::atomic<uint64_t> free_count{0};  ///< Cumulative frees back to this
                                              ///< cache.
        std::atomic<uint64_t> slab_acquire_count{0}; ///< Number of slabs
                                                     ///< acquired from
                                                     ///< SegmentProvider.
    };

    /// \brief Construct a cache for a specific size class and region.
    ///
    /// \param[in] sc Size class of blocks in this cache.
    /// \param[in] region Memory region for provenance (default kInternal).
    explicit SlabCache(SizeClass sc, RegionType region = RegionType::kInternal)
        : size_class_(sc), region_(region) {}

    /// \brief Release all slabs back to the SegmentProvider.
    ~SlabCache();

    SlabCache(const SlabCache&) = delete;
    SlabCache& operator=(const SlabCache&) = delete;
    SlabCache(SlabCache&&) = delete;
    SlabCache& operator=(SlabCache&&) = delete;

    /// \brief Allocate a block from this cache.
    ///
    /// Tries the freelist first, then bump-allocates from the current slab.
    /// Calls \c refill() to acquire a new slab when exhausted.
    ///
    /// \param[in] owner Owning actor for header stamping.
    /// \return Pointer to user data, or \c nullptr if allocation fails.
    void* allocate(ActorId owner) noexcept;

    /// \brief Free a block back to this cache.
    ///
    /// Pushes the block's AllocHeader onto the freelist for reuse.
    ///
    /// \param[in] user_ptr Pointer previously returned by \c allocate().
    void deallocate(void* user_ptr) noexcept;

    /// \brief Return the size class of blocks in this cache.
    ///
    /// \return The SizeClass.
    SizeClass size_class() const noexcept {
        return size_class_;
    }

    /// \brief Return the memory region assigned to this cache.
    ///
    /// \return The RegionType.
    RegionType region() const noexcept {
        return region_;
    }

    /// \brief Return the current live block count.
    ///
    /// \return Number of blocks currently allocated (not free).
    uint32_t live_count() const noexcept {
        return live_count_.load();
    }

    /// \brief Return a const reference to the cache statistics.
    ///
    /// \return The Stats struct.
    const Stats& stats() const noexcept {
        return stats_;
    }

  private:
    /// \brief Acquire a new slab from the SegmentProvider to refill bump space.
    void refill();

    SizeClass size_class_;
    RegionType region_{RegionType::kInternal};
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
