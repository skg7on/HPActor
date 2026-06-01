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

#include <array>
#include <atomic>
#include <cstdint>

namespace hpactor::mem {

/// \brief Typed memory regions for allocation provenance and accounting.
enum class RegionType : uint8_t {
    kActor = 0,     ///< Actor state and member containers.
    kMessage = 1,   ///< Message payloads and envelopes.
    kCoroutine = 2, ///< Coroutine frames and awaiters.
    kNetwork = 3,   ///< Network buffers and connection state.
    kInternal = 4,  ///< Internal framework allocations.
    kHibernate = 5, ///< Hibernation serialization buffers.
};

/// \brief Number of distinct memory regions.
inline constexpr uint8_t kNumRegionTypes = 6;

/// \brief Memory pressure levels returned by region admission control.
enum class MemoryPressureState : uint8_t {
    kNormal = 0,    ///< Region is below the high-water mark.
    kHigh = 1,      ///< Region is at or above the high-water mark.
    kHardLimit = 2, ///< Region has reached its hard limit; new allocations
                    ///< rejected.
};

/// \brief Per-region capacity limit configuration.
struct RegionLimit {
    uint64_t hard_limit_bytes{0}; ///< Maximum bytes allowed; 0 means unlimited.
    float high_watermark_ratio{1.0f}; ///< Fraction of hard limit that triggers
                                      ///< kHigh pressure.
};

/// \brief Per-region statistics, cache-line aligned to avoid false sharing.
struct alignas(64) RegionStats {
    std::atomic<uint64_t> total_allocated{0}; ///< Cumulative bytes allocated.
    std::atomic<uint64_t> total_freed{0};     ///< Cumulative bytes freed.
    std::atomic<uint64_t> active_bytes{0};    ///< Currently allocated bytes
                                              ///< (allocated - freed).
    std::atomic<uint64_t> high_water_mark{0}; ///< Peak active_bytes observed.
    std::atomic<uint64_t> alloc_count{0}; ///< Number of allocation operations.
    std::atomic<uint64_t> free_count{0};  ///< Number of free operations.
    std::atomic<uint64_t> corruption_events{0};    ///< Canary or guard-page
                                                   ///< violations detected.
    std::atomic<uint64_t> rejected_alloc_count{0}; ///< Allocations rejected by
                                                   ///< pressure admission.
};

/// \brief Non-atomic snapshot of region statistics for observability.
struct RegionSnapshot {
    uint64_t total_allocated{0};
    uint64_t total_freed{0};
    uint64_t active_bytes{0};
    uint64_t high_water_mark{0};
    uint64_t alloc_count{0};
    uint64_t free_count{0};
    uint64_t corruption_events{0};
    uint64_t rejected_alloc_count{0};
    RegionLimit limit{};
    MemoryPressureState pressure{MemoryPressureState::kNormal};
};

/// \brief Singleton registry for per-region accounting, pressure admission,
/// and corruption tracking.
///
/// All allocation paths call \c try_reserve / \c commit_alloc before returning
/// memory to the caller, and \c record_free on deallocation.
///
/// \note Fully lock-free: all counters use \c std::atomic with relaxed or
///       acquire-release ordering. Safe to call from any thread.
class MemoryRegionRegistry {
  public:
    /// \brief Return the singleton instance.
    static MemoryRegionRegistry& instance();

    /// \brief Try to reserve capacity for an upcoming allocation.
    ///
    /// Checks whether \p charged_bytes would exceed the region's hard limit.
    ///
    /// \param[in] region The region to charge against.
    /// \param[in] charged_bytes Number of bytes to reserve.
    /// \return \c true if the allocation is admitted, \c false if the hard
    ///         limit would be exceeded.
    bool try_reserve(RegionType region, size_t charged_bytes) noexcept;

    /// \brief Commit a previously reserved allocation to the region stats.
    ///
    /// \param[in] region The region to charge against.
    /// \param[in] charged_bytes Number of bytes to commit.
    /// \pre \c try_reserve() must have returned \c true for this charge.
    void commit_alloc(RegionType region, size_t charged_bytes) noexcept;

    /// \brief Cancel a reservation (e.g. when allocation fails after
    /// admission).
    ///
    /// \param[in] region The region the reservation was made against.
    /// \param[in] charged_bytes Number of bytes to release from the
    /// reservation.
    void cancel_reservation(RegionType region, size_t charged_bytes) noexcept;

    /// \brief Record a deallocation against region stats.
    ///
    /// \param[in] region The region to credit.
    /// \param[in] charged_bytes Number of bytes freed.
    void record_free(RegionType region, size_t charged_bytes) noexcept;

    /// \brief Record a corruption event detected in this region.
    ///
    /// \param[in] region The region where corruption was detected.
    void record_corruption(RegionType region) noexcept;

    /// \brief Set the capacity limit for a region.
    ///
    /// \param[in] region The region to configure.
    /// \param[in] limit The new limit (hard_limit_bytes and watermark ratio).
    void configure_region(RegionType region, RegionLimit limit) noexcept;

    /// \brief Return the current capacity limit for a region.
    ///
    /// \param[in] region The region to query.
    /// \return The current \c RegionLimit.
    RegionLimit limit(RegionType region) const noexcept;

    /// \brief Take a non-atomic snapshot of a region's statistics.
    ///
    /// \param[in] region The region to snapshot.
    /// \return A \c RegionSnapshot with current values and pressure state.
    /// \note Individual fields are read atomically but the snapshot as a whole
    ///       is not consistent — counters may have advanced between reads.
    RegionSnapshot snapshot(RegionType region) const noexcept;

  private:
    MemoryRegionRegistry() = default;
    static uint8_t index(RegionType region) noexcept;

    /// \brief Compute the pressure state for a region given its current active
    /// bytes.
    MemoryPressureState pressure_for(uint8_t idx, uint64_t active) const noexcept;

    std::array<RegionStats, kNumRegionTypes> stats_{};
    std::array<RegionLimit, kNumRegionTypes> limits_{};
};

} // namespace hpactor::mem
