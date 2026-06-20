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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hpactor::mem {

/// \brief Allocation strategy for a SlabCache.
enum class AllocationStrategy : uint8_t {
    kCasLifo = 0, ///< Current behavior: bump + CAS LIFO freelist (default).
    kSegregatedFit = 1, ///< Segregated free lists with bounded search
                        ///< (MEM-001).
    kBumpOnly = 2,      ///< Bump allocation only, no freelist (MEM-003).
};

/// \brief Number of segregated bins per slab.
static constexpr uint8_t kNumSegregatedBins = 8;

/// \brief Maximum search depth per bin before rotating to the next.
static constexpr uint8_t kMaxSearchDepthPerBin = 3;

/// \brief Minimum block size eligible for coalescing.
///
/// Blocks smaller than this (32B user data) cannot store the \c prev pointer
/// needed for doubly-linked free list removal (MEM-002 Appendix A).
static constexpr size_t kMinCoalesceBlockSize = 64; // 64B block = 32B user data

/// \brief Intrusive doubly-linked list linkage stored in freed block user data.
///
/// \c next reuses \c AllocHeader::next (in union with \c owner_id).
/// \c prev is stored in the first 8 bytes of the freed block's user data
/// region, which is unused and poisoned anyway — zero additional memory
/// overhead.
struct FreeBlockLinkage {
    AllocHeader* next; ///< Next free block (reuses AllocHeader::next).
    AllocHeader* prev; ///< Previous free block (stored in user data).
};

/// \brief Configuration for a single region's allocation strategy (MEM-003).
struct RegionStrategyConfig {
    AllocationStrategy strategy{AllocationStrategy::kCasLifo};
    bool enable_coalescing{false};
};

/// \brief Per-region strategy table indexed by RegionType (MEM-003).
struct MemoryStrategyTable {
    RegionStrategyConfig regions[kNumRegionTypes];
};

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
    /// \param[in] strategy Allocation strategy (default kCasLifo for backward
    ///                    compatibility).
    /// \param[in] coalescing Enable free block coalescing (MEM-002, default
    /// false).
    explicit SlabCache(SizeClass sc, RegionType region = RegionType::kInternal,
                       AllocationStrategy strategy = AllocationStrategy::kCasLifo,
                       bool coalescing = false)
        : size_class_(sc), region_(region), strategy_(strategy),
          coalescing_(coalescing) {}

    /// \brief Return the allocation strategy for this cache.
    ///
    /// \return The AllocationStrategy.
    AllocationStrategy strategy() const noexcept {
        return strategy_;
    }

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

    /// \brief Return the number of segregated bins.
    ///
    /// \return kNumSegregatedBins when strategy is kSegregatedFit, 0 otherwise.
    uint8_t bin_count() const noexcept {
        if (strategy_ == AllocationStrategy::kSegregatedFit) {
            return kNumSegregatedBins;
        }
        return 0;
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
    AllocationStrategy strategy_{AllocationStrategy::kCasLifo};
    uint8_t current_generation_{0};

    /// \brief Compute the bin index for a freed block based on its offset
    ///        within the current slab.
    uint8_t compute_bin_index(const AllocHeader* header) const noexcept;

    /// \brief Coalesce a freed block with adjacent free neighbors (MEM-002).
    ///
    /// Checks left and right neighbors. If either is free, removes them from
    /// their freelist bin and merges into a single larger free block.
    ///
    /// \param[in,out] header The block being freed (may be replaced by left
    /// neighbor).
    /// \return The start of the coalesced block.
    AllocHeader* try_coalesce(AllocHeader* header) noexcept;

    /// \brief Stamp a boundary footer at the end of a freed block.
    void stamp_boundary_footer(AllocHeader* header, size_t block_sz) noexcept;

    // ── Doubly-linked free list helpers (MEM-002 §3.3) ────────────
    //
    // Each segregated bin is a doubly-linked intrusive list threaded
    // through freed blocks. next reuses AllocHeader::next (union with
    // owner_id). prev is stored in the first 8 bytes of user data.
    // These are single-threaded: only the owning SlabCache thread
    // calls them. Cross-thread frees are routed via slab lookup.

    /// \brief Push a block to the front of a bin's free list.
    static void dll_push(FreeList<AllocHeader>& bin, AllocHeader* block) noexcept;

    /// \brief Pop a block from the front of a bin's free list.
    static AllocHeader* dll_pop(FreeList<AllocHeader>& bin) noexcept;

    /// \brief Remove a specific block from anywhere in a bin's free list.
    static void dll_remove(FreeList<AllocHeader>& bin, AllocHeader* block) noexcept;

    bool coalescing_{false};
    std::byte* current_slab_{nullptr};
    size_t slab_size_{0};
    size_t bump_offset_{0};
    uint32_t bin_stride_bytes_{0};
    uint8_t start_bin_{0};

    FreeList<AllocHeader> freelist_;
    std::array<FreeList<AllocHeader>, kNumSegregatedBins> bins_;
    std::atomic<uint32_t> live_count_{0};
    Stats stats_;

    /// \brief Fully-freed slabs available for reuse (bump-only idle recycling).
    ///
    /// When a bump-only slab's live_count reaches 0, the slab is moved here
    /// instead of being leaked. On next allocation exhaustion, these slabs are
    /// reused before calling SegmentProvider (MEM-003 §3.3).
    std::vector<std::byte*> idle_slabs_;

    /// \brief All slabs currently owned (active, idle, and bump-in-progress).
    std::vector<std::byte*> slabs_;
};

} // namespace hpactor::mem
