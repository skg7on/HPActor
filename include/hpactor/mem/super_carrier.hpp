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
    size_t huge_page_size{0};
};

/// \brief Probe the system for huge page support.
///
/// Tries \c mmap(MAP_HUGETLB) and checks \c /proc/sys/vm/nr_hugepages.
/// Safe to call at any time; caches nothing.
///
/// \return HugePageInfo indicating what page sizes are available.
HugePageInfo probe_huge_pages() noexcept;

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
    /// \brief Initialize the super carrier reservation.
    ///
    /// Calls \c mmap(PROT_NONE, MAP_NORESERVE) to reserve virtual address space
    /// without committing physical pages. When \p huge_info indicates huge page
    /// availability, \c MAP_HUGETLB is used (MEM-005).
    ///
    /// \param[in] size_bytes Size of the virtual reservation.
    /// \param[in] huge_info Result from \c probe_huge_pages(). Pass a
    ///            default-constructed \c HugePageInfo to use standard pages.
    /// \return \c true on success, \c false if \c mmap fails or carrier is
    ///         disabled on this platform.
    bool init(size_t size_bytes,
              const HugePageInfo& huge_info = HugePageInfo{}) noexcept;

    /// \brief Carve a slab from the carrier.
    ///
    /// \c mprotect's a slice from PROT_NONE to PROT_READ|PROT_WRITE.
    ///
    /// \param[in] slab_size_bytes Size of the slab to carve.
    /// \return Pointer to the writable slab memory, or \c nullptr if the
    ///         carrier is exhausted.
    void* carve(size_t slab_size_bytes) noexcept;

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

    /// \brief The base address of the carrier reservation.
    [[nodiscard]] const void* carrier_base() const noexcept {
        return carrier_base_;
    }

  private:
    void* carrier_base_{nullptr};
    size_t carrier_size_{0};
    std::atomic<size_t> carve_offset_{0};
    std::atomic<size_t> released_bytes_{0};
};

} // namespace hpactor::mem
