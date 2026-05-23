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

enum class RegionType : uint8_t {
    kActor = 0,
    kMessage = 1,
    kCoroutine = 2,
    kNetwork = 3,
    kInternal = 4,
    kHibernate = 5,
};

inline constexpr uint8_t kNumRegionTypes = 6;

enum class MemoryPressureState : uint8_t {
    kNormal = 0,
    kHigh = 1,
    kHardLimit = 2,
};

struct RegionLimit {
    uint64_t hard_limit_bytes{0}; // 0 means unlimited
    float high_watermark_ratio{1.0f};
};

// Per-region statistics (cache-line aligned to avoid false sharing)
struct alignas(64) RegionStats {
    std::atomic<uint64_t> total_allocated{0};
    std::atomic<uint64_t> total_freed{0};
    std::atomic<uint64_t> active_bytes{0};
    std::atomic<uint64_t> high_water_mark{0};
    std::atomic<uint64_t> alloc_count{0};
    std::atomic<uint64_t> free_count{0};
    std::atomic<uint64_t> corruption_events{0};
    std::atomic<uint64_t> rejected_alloc_count{0};
};

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

class MemoryRegionRegistry {
  public:
    static MemoryRegionRegistry& instance();

    bool try_reserve(RegionType region, size_t charged_bytes) noexcept;
    void commit_alloc(RegionType region, size_t charged_bytes) noexcept;
    void cancel_reservation(RegionType region, size_t charged_bytes) noexcept;
    void record_free(RegionType region, size_t charged_bytes) noexcept;
    void record_corruption(RegionType region) noexcept;

    void configure_region(RegionType region, RegionLimit limit) noexcept;
    RegionLimit limit(RegionType region) const noexcept;
    RegionSnapshot snapshot(RegionType region) const noexcept;

  private:
    MemoryRegionRegistry() = default;
    static uint8_t index(RegionType region) noexcept;
    MemoryPressureState pressure_for(uint8_t idx, uint64_t active) const noexcept;

    std::array<RegionStats, kNumRegionTypes> stats_{};
    std::array<RegionLimit, kNumRegionTypes> limits_{};
};

} // namespace hpactor::mem
