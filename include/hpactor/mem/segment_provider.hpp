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
#include <hpactor/mem/super_carrier.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hpactor::mem {

class SuperCarrier; // forward declaration

/// \brief Tier-0 global segment provider.
///
/// Acquires large mmap'd regions (2 MB segments) and carves them into slabs
/// for thread-local caches. Owns all segment memory and tracks slab-to-cache
/// assignments for cross-thread free routing.
///
/// \note Thread-safe: all public methods acquire an internal mutex. This is
///       not on the hot path — slab acquisition happens only when a
///       thread-local cache is exhausted.
class SegmentProvider {
  public:
    /// \brief Result of a segment lookup by pointer.
    struct SegmentInfo {
        void* base;  ///< Base address of the containing segment.
        size_t size; ///< Total size of the containing segment.
    };

    /// \brief Result of a slab metadata lookup (interior-pointer aware).
    struct SlabInfo {
        void* segment_base{nullptr}; ///< Base of the owning segment.
        void* slab_base{nullptr};    ///< Base of the slab.
        size_t segment_size{0};      ///< Total segment size.
        size_t slab_size{0};         ///< Size of this slab.
        void* owner_cache{nullptr};  ///< SlabCache that owns this slab (for
                                     ///< cross-thread frees).
        RegionType region{RegionType::kInternal}; ///< Region this slab is
                                                  ///< assigned to.
        SizeClass size_class{SizeClass::k32B}; ///< Size class of blocks in this
                                               ///< slab.
        bool found{false}; ///< \c true if the lookup matched a known slab.
    };

    /// \brief Aggregate segment provider statistics.
    struct Stats {
        size_t total_allocated{0};      ///< Total bytes mmap'd.
        size_t active_segments{0};      ///< Number of live segments.
        uint64_t huge_page_segments{0}; ///< Segments with MAP_HUGETLB
                                        ///< (MEM-005).
        uint64_t thp_segments{0};       ///< Segments with MADV_HUGEPAGE hint.
        uint64_t regular_segments{0};   ///< Segments with 4KB pages.
    };

    /// \brief Return the singleton instance.
    static SegmentProvider& instance();

    /// \brief Acquire a slab of the given size class.
    ///
    /// Carves from an existing segment or allocates a new one.
    ///
    /// \param[in] sc The size class for blocks in this slab.
    /// \return Base pointer of the new slab.
    void* acquire_slab(SizeClass sc);

    /// \brief Release a slab back to the provider.
    ///
    /// When all slabs in a segment are freed, the segment is munmap'd.
    ///
    /// \param[in] slab Base pointer returned by \c acquire_slab().
    /// \param[in] sc The size class that was passed to \c acquire_slab().
    void release_slab(void* slab, SizeClass sc);

    /// \brief Look up which segment contains a pointer.
    ///
    /// \param[in] ptr Any pointer into a managed segment.
    /// \return SegmentInfo for the containing segment.
    SegmentInfo lookup(void* ptr) const;

    /// \brief Register slab ownership metadata for cross-thread free routing.
    ///
    /// \param[in] slab Base pointer of the slab.
    /// \param[in] slab_size Size of the slab in bytes.
    /// \param[in] owner_cache Pointer to the SlabCache that owns this slab.
    /// \param[in] region RegionType assigned to this slab.
    /// \param[in] sc SizeClass of blocks in this slab.
    void register_slab_owner(void* slab, size_t slab_size, void* owner_cache,
                             RegionType region, SizeClass sc);

    /// \brief Look up slab metadata for a pointer (interior-pointer aware).
    ///
    /// The pointer may point anywhere within a slab, not just at its base.
    ///
    /// \param[in] ptr Any pointer.
    /// \return SlabInfo with \c found == \c true if the pointer falls within
    ///         a known slab.
    SlabInfo lookup_slab(void* ptr) const;

    /// \brief Return the slab size for a given size class.
    ///
    /// \param[in] sc The size class.
    /// \return Size of a full slab in bytes.
    size_t slab_size(SizeClass sc) const;

    /// \brief Return aggregate statistics.
    ///
    /// \return A snapshot of current segment provider stats.
    Stats stats() const;

    /// \brief Set the super carrier for slab carving (MEM-004 §3.2).
    ///
    /// When set, \c acquire_slab() will attempt to carve from the carrier
    /// before falling back to individual mmap segments.
    ///
    /// \param[in] carrier Pointer to an initialized SuperCarrier, or nullptr.
    void set_super_carrier(SuperCarrier* carrier) noexcept {
        super_carrier_ = carrier;
    }

    /// \brief Return the current super carrier, or nullptr if not set.
    [[nodiscard]] SuperCarrier* super_carrier() const noexcept {
        return super_carrier_;
    }

    /// \brief Set huge page info for legacy segment allocation (MEM-005 §3.4).
    ///
    /// When huge pages are available, \c allocate_new_segment() will attempt
    /// \c MAP_HUGETLB before falling back to standard pages.
    ///
    /// \param[in] info Result from \c probe_huge_pages().
    void set_huge_page_info(const HugePageInfo& info) noexcept {
        huge_info_ = info;
    }

  private:
    SegmentProvider() = default;

    static constexpr size_t kSegmentSize = 2 * 1024 * 1024; // 2MB
    static constexpr size_t kBaseSlabSize = 64 * 1024;      // 64KB default

    struct Segment {
        void* base{nullptr};
        size_t size{0};
        size_t offset{0};
        uint32_t ref_count{0}; // atomic not needed; protected by mutex_

        void inc_ref() {
            ++ref_count;
        }
        uint32_t dec_ref() {
            return --ref_count;
        }
    };

    struct SlabRecord {
        uint32_t segment_index{0};
        size_t slab_size_bytes{0};
        void* owner_cache{nullptr};
        RegionType region{RegionType::kInternal};
        SizeClass size_class{SizeClass::k32B};
    };

    void* carve_from_segment(SizeClass sc);
    void* allocate_new_segment(size_t size);

    mutable std::mutex mutex_;
    std::vector<Segment> segments_;
    std::unordered_map<void*, SlabRecord> slab_records_;
    std::atomic<SuperCarrier*> super_carrier_{nullptr}; ///< Optional carrier
                                                        ///< (MEM-004). Fix #9:
                                                        ///< atomic for
                                                        ///< lock-free read in
                                                        ///< acquire_slab.
    HugePageInfo huge_info_{};            ///< Huge page config (MEM-005).
    mutable uint64_t huge_page_count_{0}; ///< MAP_HUGETLB segments (MEM-005).
    mutable uint64_t thp_count_{0};       ///< MADV_HUGEPAGE segments (MEM-005).
    mutable uint64_t regular_count_{0};   ///< 4KB segments (MEM-005).
};

} // namespace hpactor::mem
