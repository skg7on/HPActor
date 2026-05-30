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

#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/mem/memory_region.hpp>

namespace hpactor::mem {

MemoryRegionRegistry& MemoryRegionRegistry::instance() {
    static MemoryRegionRegistry registry;
    return registry;
}

uint8_t MemoryRegionRegistry::index(RegionType region) noexcept {
    return static_cast<uint8_t>(region);
}

MemoryPressureState
MemoryRegionRegistry::pressure_for(uint8_t idx, uint64_t active) const noexcept {
    const auto& limit = limits_[idx];
    if (limit.hard_limit_bytes != 0 && active >= limit.hard_limit_bytes) {
        return MemoryPressureState::kHardLimit;
    }
    if (limit.hard_limit_bytes != 0) {
        uint64_t high_water =
            static_cast<uint64_t>(static_cast<double>(limit.hard_limit_bytes) *
                                  static_cast<double>(limit.high_watermark_ratio));
        if (active >= high_water) {
            return MemoryPressureState::kHigh;
        }
    }
    return MemoryPressureState::kNormal;
}

bool MemoryRegionRegistry::try_reserve(RegionType region,
                                       size_t charged_bytes) noexcept {
    FAULT_INJECT("hpactor.allocator.region.try_reserve.fail") {
        return false;
    }

    const uint8_t idx = index(region);
    auto& stats = stats_[idx];
    const auto limit_cfg = limits_[idx];

    uint64_t current = stats.active_bytes.load(std::memory_order_relaxed);
    for (;;) {
        const uint64_t projected = current + charged_bytes;
        if (limit_cfg.hard_limit_bytes != 0 &&
            projected > limit_cfg.hard_limit_bytes) {
            stats.rejected_alloc_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (stats.active_bytes.compare_exchange_weak(current, projected,
                                                     std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {
            uint64_t peak = stats.high_water_mark.load(std::memory_order_relaxed);
            while (projected > peak &&
                   !stats.high_water_mark.compare_exchange_weak(
                       peak, projected, std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
            return true;
        }
    }
}

void MemoryRegionRegistry::commit_alloc(RegionType region,
                                        size_t charged_bytes) noexcept {
    const uint8_t idx = index(region);
    auto& stats = stats_[idx];
    stats.total_allocated.fetch_add(charged_bytes, std::memory_order_relaxed);
    stats.alloc_count.fetch_add(1, std::memory_order_relaxed);
}

void MemoryRegionRegistry::cancel_reservation(RegionType region,
                                              size_t charged_bytes) noexcept {
    const uint8_t idx = index(region);
    stats_[idx].active_bytes.fetch_sub(charged_bytes, std::memory_order_relaxed);
}

void MemoryRegionRegistry::record_free(RegionType region,
                                       size_t charged_bytes) noexcept {
    FAULT_INJECT("hpactor.allocator.region.record_free.skip") {
        return;
    }
    const uint8_t idx = index(region);
    auto& stats = stats_[idx];
    stats.active_bytes.fetch_sub(charged_bytes, std::memory_order_relaxed);
    stats.total_freed.fetch_add(charged_bytes, std::memory_order_relaxed);
    stats.free_count.fetch_add(1, std::memory_order_relaxed);
}

void MemoryRegionRegistry::record_corruption(RegionType region) noexcept {
    const uint8_t idx = index(region);
    stats_[idx].corruption_events.fetch_add(1, std::memory_order_relaxed);
}

void MemoryRegionRegistry::configure_region(RegionType region,
                                            RegionLimit limit) noexcept {
    const uint8_t idx = index(region);
    limits_[idx] = limit;
}

RegionLimit MemoryRegionRegistry::limit(RegionType region) const noexcept {
    const uint8_t idx = index(region);
    return limits_[idx];
}

RegionSnapshot MemoryRegionRegistry::snapshot(RegionType region) const noexcept {
    const uint8_t idx = index(region);
    const auto& stats = stats_[idx];
    const uint64_t active = stats.active_bytes.load(std::memory_order_relaxed);
    RegionSnapshot snap;
    snap.total_allocated = stats.total_allocated.load(std::memory_order_relaxed);
    snap.total_freed = stats.total_freed.load(std::memory_order_relaxed);
    snap.active_bytes = active;
    snap.high_water_mark = stats.high_water_mark.load(std::memory_order_relaxed);
    snap.alloc_count = stats.alloc_count.load(std::memory_order_relaxed);
    snap.free_count = stats.free_count.load(std::memory_order_relaxed);
    snap.corruption_events =
        stats.corruption_events.load(std::memory_order_relaxed);
    snap.rejected_alloc_count =
        stats.rejected_alloc_count.load(std::memory_order_relaxed);
    snap.limit = limits_[idx];
    snap.pressure = pressure_for(idx, active);
    return snap;
}

} // namespace hpactor::mem
