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

#include <hpactor/mem/compaction.hpp>

#include <chrono>

namespace hpactor::mem {

bool CompactionManager::should_compact_slab(uint32_t live_blocks,
                                             uint32_t total_blocks) const noexcept {
    if (total_blocks == 0) return false;
    float utilization = static_cast<float>(live_blocks)
                        / static_cast<float>(total_blocks);
    return utilization <= config_.compaction_threshold;
}

CompactionManager::WasteReport
CompactionManager::compute_waste(const SlabCache& cache) noexcept {
    WasteReport report;

    // Waste = unused bytes in partially-filled slabs
    // For simplicity: report stats from the cache's built-in counters
    const auto& stats = cache.stats();
    report.total_bytes = stats.alloc_count.load(std::memory_order_relaxed)
                         * size_for_class(cache.size_class());

    // Estimate waste from freed blocks not yet recycled
    uint64_t freed = stats.free_count.load(std::memory_order_relaxed);
    uint64_t allocated = stats.alloc_count.load(std::memory_order_relaxed);
    if (allocated > freed) {
        uint64_t live = allocated - freed;
        // Slabs have fixed total blocks; compute wasted space
        // Blocks per slab for each size class (64KB base / block_size)
        static constexpr uint32_t kSlabBlocks[kNumSizeClasses] = {
            2048, 1024, 512, 512, 512, 256, 256, 128
        };
        uint64_t slab_blocks = kSlabBlocks[static_cast<uint8_t>(cache.size_class())];
        uint64_t slabs_needed = (live + slab_blocks - 1) / slab_blocks;
        uint64_t full_capacity = slabs_needed * slab_blocks
                                 * size_for_class(cache.size_class());
        report.wasted_bytes = (full_capacity > report.total_bytes)
                              ? (full_capacity - report.total_bytes) : 0;
    }

    if (report.total_bytes > 0) {
        report.waste_ratio = static_cast<float>(report.wasted_bytes)
                             / static_cast<float>(report.total_bytes);
    }
    return report;
}

bool CompactionManager::should_compact() const noexcept {
    auto now = std::chrono::steady_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return (now_ms - last_compaction_ts_)
           >= static_cast<int64_t>(config_.compaction_interval_ms);
}

void CompactionManager::record_compaction() noexcept {
    auto now = std::chrono::steady_clock::now();
    last_compaction_ts_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

} // namespace hpactor::mem
