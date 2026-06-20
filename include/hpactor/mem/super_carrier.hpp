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

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hpactor::mem {

/// \brief Result of huge page availability probing (MEM-005).
struct HugePageInfo {
    bool explicit_huge_pages_available{false};
    bool transparent_huge_pages_available{false};
    size_t huge_page_size{0};     ///< Default huge page size (typically 2MB).
    size_t huge_page_size_1gb{0}; ///< 1GB if available, 0 otherwise.
};

/// \brief Probe the system for huge page support.
///
/// Tries \c mmap(MAP_HUGETLB) and checks \c /proc/sys/vm/nr_hugepages.
/// Safe to call at any time; caches nothing.
///
/// \return HugePageInfo indicating what page sizes are available.
HugePageInfo probe_huge_pages() noexcept;

/// \brief NUMA topology information (MEM-007).
struct NumaInfo {
    /// Number of NUMA nodes detected (1 on UMA systems or if detection fails).
    uint32_t node_count{1};
    /// Whether the system is actually NUMA (node_count > 1).
    [[nodiscard]] bool is_numa() const noexcept {
        return node_count > 1;
    }
};

/// \brief Probe the system for NUMA topology.
///
/// Uses \c getcpu() on Linux to determine the maximum NUMA node index.
/// Returns \c NumaInfo{1} on non-Linux or UMA systems.
///
/// \return NumaInfo with the detected node count.
NumaInfo probe_numa_topology() noexcept;

/// \brief Return the NUMA node index of the calling thread.
///
/// Uses \c getcpu() on Linux. Returns 0 on non-Linux or if detection fails.
///
/// \return NUMA node index (0-based).
unsigned get_current_numa_node() noexcept;

/// \brief Contiguous virtual memory reservation used as primary slab source
///        (MEM-004).
///
/// Reserves a large contiguous virtual address range at ActorSystem startup
/// (configurable, typically 8–64 GB). All slabs are carved from this range via
/// \c mprotect, eliminating per-slab \c mmap / \c munmap syscalls.
///
/// \note 64-bit only. On 32-bit systems, the carrier is disabled and
///       SegmentProvider falls back to individual \c mmap segments.
/// \note Thread safety: carve() and release() use atomic offsets — safe for
///       concurrent calls from worker threads during refill.
class SuperCarrier {
  public:
    /// \brief Maximum supported NUMA nodes (MEM-007).
    static constexpr uint32_t kMaxNumaNodes = 16;

    /// \brief Initialize the super carrier reservation.
    ///
    /// Calls \c mmap(PROT_NONE, MAP_NORESERVE) to reserve virtual address space
    /// without committing physical pages. When \p huge_info indicates huge page
    /// availability, \c MAP_HUGETLB is used (MEM-005).
    ///
    /// \param[in] size_bytes Size of the virtual reservation.
    /// \param[in] huge_info Result from \c probe_huge_pages(). Pass a
    ///            default-constructed \c HugePageInfo to use standard pages.
    /// \param[in] numa_info Result from \c probe_numa_topology(). When
    /// node_count
    ///            > 1, the carrier is partitioned into per-node sub-regions
    ///            (MEM-007).
    /// \return \c true on success, \c false if \c mmap fails or carrier is
    ///         disabled on this platform.
    bool init(size_t size_bytes, const HugePageInfo& huge_info = HugePageInfo{},
              const NumaInfo& numa_info = NumaInfo{}) noexcept;

    /// \brief Carve a slab from the carrier (node-agnostic — first available).
    ///
    /// \c mprotect's a slice from PROT_NONE to PROT_READ|PROT_WRITE.
    ///
    /// \param[in] slab_size_bytes Size of the slab to carve.
    /// \return Pointer to the writable slab memory, or \c nullptr if the
    ///         carrier is exhausted.
    void* carve(size_t slab_size_bytes) noexcept;

    /// \brief Carve a slab from a specific NUMA node's sub-region (MEM-007).
    ///
    /// On UMA systems or when \p numa_node is out of range, falls back to the
    /// global carve region. Prefer this when the calling thread has known
    /// NUMA affinity to avoid cross-node memory access penalties.
    ///
    /// \param[in] slab_size_bytes Size of the slab to carve.
    /// \param[in] numa_node NUMA node to prefer (0-based).
    /// \return Pointer to the writable slab memory, or \c nullptr if the
    ///         carrier is exhausted.
    void* carve_numa(size_t slab_size_bytes, unsigned numa_node) noexcept;

    /// \brief Release a slab's physical pages back to the OS.
    ///
    /// \c madvise(MADV_FREE) + \c mprotect(PROT_NONE). The virtual reservation
    /// is preserved.
    ///
    /// \param[in] slab_addr Address previously returned by \c carve().
    /// \param[in] slab_size_bytes Size of the slab.
    void release(void* slab_addr, size_t slab_size_bytes) noexcept;

    /// \brief Whether the carrier was successfully initialized.
    [[nodiscard]] bool is_initialized() const noexcept {
        return carrier_base_ != nullptr;
    }

    /// \brief Total virtual address space reserved (bytes).
    [[nodiscard]] size_t total_reserved() const noexcept {
        return carrier_size_;
    }

    /// \brief Cumulative bytes carved since initialization.
    [[nodiscard]] size_t total_carved() const noexcept {
        return carve_offset_.load(std::memory_order_relaxed);
    }

    /// \brief Currently active carved bytes (carved - released).
    [[nodiscard]] size_t current_carved() const noexcept {
        return carve_offset_.load(std::memory_order_relaxed) -
               released_bytes_.load(std::memory_order_relaxed);
    }

    /// \brief Cumulative bytes released back to the OS.
    [[nodiscard]] size_t total_released() const noexcept {
        return released_bytes_.load(std::memory_order_relaxed);
    }

    /// \brief The base address of the carrier reservation.
    [[nodiscard]] const void* carrier_base() const noexcept {
        return carrier_base_;
    }

    /// \brief Attempt to grow the carrier via mremap (Linux only).
    ///
    /// \param[in] additional_bytes Number of bytes to add.
    /// \return \c true if growth succeeded.
    [[nodiscard]] bool grow(size_t additional_bytes) noexcept;

    /// \brief Set the maximum carrier size for growth attempts.
    void set_max_size(size_t max_bytes) noexcept {
        max_carrier_size_ = max_bytes;
    }

    /// \brief Enable or disable carrier growth via mremap.
    void set_can_grow(bool can) noexcept {
        can_grow_ = can;
    }

  private:
    /// \brief Carve from an atomic offset with exhaustion check.
    ///
    /// \param[in,out] offset Atomic offset to advance.
    /// \param[in] region_base Base offset within the carrier for this region.
    /// \param[in] region_end End offset (region_base + region_size).
    /// \param[in] slab_size_bytes Size of the slab to carve.
    void* carve_from_offset(std::atomic<size_t>& offset, size_t region_base,
                            size_t region_end, size_t slab_size_bytes) noexcept;

    void* carrier_base_{nullptr};
    size_t carrier_size_{0};
    std::atomic<size_t> carve_offset_{0};
    std::atomic<size_t> released_bytes_{0};
    size_t max_carrier_size_{0}; ///< Configurable growth cap (default:
                                 ///< unlimited).
    bool can_grow_{true};        ///< Allow mremap growth (Linux only).

    // MEM-007: NUMA per-node sub-regions
    uint32_t numa_node_count_{1};
    std::atomic<size_t> numa_carve_offsets_[kMaxNumaNodes]{};
    size_t numa_node_size_{0}; ///< bytes per NUMA node sub-region
};

} // namespace hpactor::mem
