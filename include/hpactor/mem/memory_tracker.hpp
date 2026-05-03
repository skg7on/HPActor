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

// Per-actor memory statistics. Cache-line aligned to eliminate false sharing.
struct alignas(64) ActorMemoryStats {
    uint64_t current_bytes{0};
    uint64_t peak_bytes{0};
    uint64_t alloc_count{0};
    uint64_t free_count{0};
    uint64_t last_alloc_ns{0};
    uint64_t _padding[3]{0}; // pad to 64 bytes
};

// Global array of per-actor memory counters, indexed by ActorId.
// Uses pre-allocated fixed-size array to avoid issues with atomic move/copy.
class MemoryTracker {
  public:
    static constexpr size_t kMaxTrackedActors = 1'000'000;

    static MemoryTracker& instance();

    // Record an allocation. Returns false if actor index out of range.
    bool record_alloc(ActorId actor, size_t bytes) noexcept;

    // Record a deallocation.
    void record_free(ActorId actor, size_t bytes) noexcept;

    // Read stable stats for an actor (relaxed — snapshot consistency not guaranteed).
    void snapshot(ActorId actor, ActorMemoryStats& out) const noexcept;

    // Global aggregates across all tracked actors.
    uint64_t total_active_bytes() const noexcept;
    uint64_t total_peak_bytes() const noexcept;
    uint64_t total_alloc_count() const noexcept;

    // Direct access to the stats array for bulk operations.
    const ActorMemoryStats* data() const noexcept { return stats_.get(); }
    size_t capacity() const noexcept { return capacity_; }

  private:
    MemoryTracker();

    size_t index_for(ActorId actor) const noexcept;

    std::unique_ptr<ActorMemoryStats[]> stats_;
    size_t capacity_{0};
};

} // namespace hpactor::mem
