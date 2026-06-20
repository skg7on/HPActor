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

/// \brief Per-thread allocator.
///
/// Owns a SlabCache for each (RegionType × SizeClass) combination — a matrix
/// of \c kNumRegionTypes rows × \c kNumSizeClasses columns. The hot allocation
/// path is bump-pointer or freelist pop with no locks.
///
/// \note Thread-confined: each instance is owned by exactly one WorkerThread.
///       Cross-thread frees are routed to the origin SlabCache via
///       SegmentProvider slab lookup.
class ThreadLocalAllocator {
  public:
    /// \brief Construct all per-region, per-size-class caches with the default
    ///        strategy (kCasLifo, no coalescing — backward compatible).
    ThreadLocalAllocator();

    /// \brief Construct all caches using the given strategy table (MEM-003).
    ///
    /// \param[in] table Per-region strategy and coalescing configuration.
    explicit ThreadLocalAllocator(const MemoryStrategyTable& table);

    /// \brief Release all caches and their slabs.
    ~ThreadLocalAllocator();

    ThreadLocalAllocator(const ThreadLocalAllocator&) = delete;
    ThreadLocalAllocator& operator=(const ThreadLocalAllocator&) = delete;
    ThreadLocalAllocator(ThreadLocalAllocator&&) = delete;
    ThreadLocalAllocator& operator=(ThreadLocalAllocator&&) = delete;

    /// \brief Allocate from a specific region and size class.
    ///
    /// \param[in] region Memory region to charge.
    /// \param[in] sc Size class for the requested block size.
    /// \param[in] owner Owning actor.
    /// \return Pointer to user data, or \c nullptr on failure.
    void* allocate(RegionType region, SizeClass sc, ActorId owner) noexcept;

    /// \brief Allocate from a specific region by user-requested byte size.
    ///
    /// Maps \p user_bytes to the appropriate SizeClass internally.
    ///
    /// \param[in] region Memory region to charge.
    /// \param[in] user_bytes Number of bytes requested.
    /// \param[in] owner Owning actor.
    /// \return Pointer to user data, or \c nullptr on failure.
    void* allocate(RegionType region, size_t user_bytes, ActorId owner) noexcept;

    /// \brief Backward-compatible: allocate with default region (kInternal).
    ///
    /// \param[in] sc Size class.
    /// \param[in] owner Owning actor.
    /// \return Pointer to user data, or \c nullptr on failure.
    void* allocate(SizeClass sc, ActorId owner) noexcept {
        return allocate(RegionType::kInternal, sc, owner);
    }

    /// \brief Backward-compatible: allocate by bytes with default region.
    ///
    /// \param[in] user_bytes Number of bytes requested.
    /// \param[in] owner Owning actor.
    /// \return Pointer to user data, or \c nullptr on failure.
    void* allocate_bytes(size_t user_bytes, ActorId owner) noexcept {
        return allocate(RegionType::kInternal, user_bytes, owner);
    }

    /// \brief Deallocate a block.
    ///
    /// Routes to the origin SlabCache for cross-thread frees via slab lookup.
    ///
    /// \param[in] user_ptr Pointer previously returned by an \c allocate()
    /// overload.
    void deallocate(void* user_ptr) noexcept;

    /// \brief Return stats for a specific size class (default region).
    ///
    /// \param[in] sc Size class.
    /// \return Const reference to the cache's Stats.
    const SlabCache::Stats& stats(SizeClass sc) const noexcept;

    /// \brief Return stats for a specific region and size class.
    ///
    /// \param[in] region Memory region.
    /// \param[in] sc Size class.
    /// \return Const reference to the cache's Stats.
    const SlabCache::Stats& stats(RegionType region, SizeClass sc) const noexcept;

  private:
    using CacheRow = std::array<SlabCache*, kNumSizeClasses>;
    std::array<CacheRow, kNumRegionTypes> caches_{};
};

} // namespace hpactor::mem
