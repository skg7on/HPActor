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

#include <hpactor/mem/slab_cache.hpp>

#include <cstddef>
#include <cstdint>

namespace hpactor::mem {

// Per-slab fragmentation tracking info.
struct SlabCompactionInfo {
    uint64_t generation{0};
    uint32_t total_blocks{0};
    uint32_t compaction_threshold_blocks{0};

    // Returns true if this slab should be compacted.
    // Threshold: compact when live/total ratio drops below 25%.
    bool should_compact(uint32_t live_count) const noexcept {
        if (total_blocks == 0) return false;
        return live_count <= compaction_threshold_blocks;
    }

    float utilization(uint32_t live_count) const noexcept {
        if (total_blocks == 0) return 0.0f;
        return static_cast<float>(live_count) / static_cast<float>(total_blocks);
    }
};

// Compaction configuration.
struct CompactionConfig {
    float fragmentation_budget = 0.05f;
    float compaction_threshold = 0.25f;
    uint64_t compaction_interval_ms = 60000;
};

// Manages compaction across all slab caches.
// Tracks fragmentation and triggers compaction when the waste budget
// exceeds a configurable threshold (default 5%).
class CompactionManager {
  public:
    explicit CompactionManager(const CompactionConfig& cfg = {}) : config_(cfg) {}

    // Check if a slab should be compacted based on utilization.
    bool should_compact_slab(uint32_t live_blocks, uint32_t total_blocks) const noexcept;

    // Calculate total fragmentation waste across slabs.
    // Returns (wasted_bytes, total_bytes).
    struct WasteReport {
        size_t wasted_bytes{0};
        size_t total_bytes{0};
        float waste_ratio{0.0f};
    };
    static WasteReport compute_waste(const SlabCache& cache) noexcept;

    // Check whether compaction should run now.
    bool should_compact() const noexcept;

    // Record that a compaction cycle ran.
    void record_compaction() noexcept;

    // Current config
    const CompactionConfig& config() const noexcept { return config_; }

  private:
    CompactionConfig config_;
    int64_t last_compaction_ts_{0};
};

} // namespace hpactor::mem
