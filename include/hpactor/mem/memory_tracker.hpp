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

#include <hpactor/types/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace hpactor::mem {

/// \brief Per-actor memory statistics, cache-line aligned to avoid false
/// sharing.
struct alignas(64) ActorMemoryStats {
    uint64_t current_bytes{0}; ///< Currently allocated bytes.
    uint64_t peak_bytes{0};    ///< Peak current_bytes observed.
    uint64_t alloc_count{0};   ///< Cumulative allocation count.
    uint64_t free_count{0};    ///< Cumulative deallocation count.
    uint64_t last_alloc_ns{0}; ///< Timestamp of the most recent allocation
                               ///< (ns).
    uint64_t _padding[3]{0};   ///< Padding to 64 B cache-line boundary.
};

/// \brief Global array of per-actor memory counters, indexed by ActorId.
///
/// Uses a pre-allocated fixed-size array to avoid atomic move/copy issues and
/// to keep all counters in a single contiguous allocation for cache efficiency.
///
/// \note Counters are updated with relaxed atomics. Snapshots read individual
///       fields atomically but are not point-in-time consistent across fields.
class MemoryTracker {
  public:
    /// \brief Maximum number of actors that can be tracked.
    static constexpr size_t kMaxTrackedActors = 1'000'000;

    /// \brief Return the singleton instance.
    static MemoryTracker& instance();

    /// \brief Record an allocation for an actor.
    ///
    /// \param[in] actor The actor that owns this allocation.
    /// \param[in] bytes Number of bytes allocated.
    /// \return \c false if the actor index is out of range, \c true otherwise.
    bool record_alloc(ActorId actor, size_t bytes) noexcept;

    /// \brief Record a deallocation for an actor.
    ///
    /// \param[in] actor The actor that owned the freed memory.
    /// \param[in] bytes Number of bytes freed.
    void record_free(ActorId actor, size_t bytes) noexcept;

    /// \brief Read stable stats for an actor.
    ///
    /// \param[in] actor The actor to query.
    /// \param[out] out Destination for the snapshot.
    /// \note Individual fields are read atomically but the snapshot as a whole
    ///       is not guaranteed to be point-in-time consistent.
    void snapshot(ActorId actor, ActorMemoryStats& out) const noexcept;

    /// \brief Sum of current_bytes across all tracked actors.
    ///
    /// \return Total active bytes.
    uint64_t total_active_bytes() const noexcept;

    /// \brief Sum of peak_bytes across all tracked actors.
    ///
    /// \return Total peak bytes.
    uint64_t total_peak_bytes() const noexcept;

    /// \brief Sum of alloc_count across all tracked actors.
    ///
    /// \return Total allocation operations.
    uint64_t total_alloc_count() const noexcept;

    /// \brief Direct access to the stats array for bulk operations.
    ///
    /// \return Pointer to the start of the contiguous stats array.
    const ActorMemoryStats* data() const noexcept {
        return stats_.get();
    }

    /// \brief Number of tracked-actor slots.
    ///
    /// \return The array capacity (may be larger than the number of active
    /// actors).
    size_t capacity() const noexcept {
        return capacity_;
    }

  private:
    MemoryTracker();

    /// \brief Map an ActorId to an index into the stats array.
    size_t index_for(ActorId actor) const noexcept;

    std::unique_ptr<ActorMemoryStats[]> stats_;
    size_t capacity_{0};
};

} // namespace hpactor::mem
