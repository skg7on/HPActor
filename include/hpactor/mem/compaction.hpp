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

/// \brief Per-slab fragmentation tracking metadata.
struct SlabCompactionInfo {
    uint64_t generation{0};                  ///< Compaction generation counter.
    uint32_t total_blocks{0};                ///< Total blocks in this slab.
    uint32_t compaction_threshold_blocks{0}; ///< Block count below which
                                             ///< compaction triggers.

    /// \brief Check whether this slab should be compacted.
    ///
    /// Compaction is recommended when live/total ratio drops below 25%.
    ///
    /// \param[in] live_count Currently live blocks in this slab.
    /// \return \c true if the slab is a compaction candidate.
    bool should_compact(uint32_t live_count) const noexcept {
        if (total_blocks == 0)
            return false;
        return live_count <= compaction_threshold_blocks;
    }

    /// \brief Compute the current utilization ratio of this slab.
    ///
    /// \param[in] live_count Currently live blocks.
    /// \return Ratio in [0.0, 1.0]; 0 if the slab has no blocks.
    float utilization(uint32_t live_count) const noexcept {
        if (total_blocks == 0)
            return 0.0f;
        return static_cast<float>(live_count) / static_cast<float>(total_blocks);
    }
};

/// \brief Configuration for the compaction subsystem.
struct CompactionConfig {
    float fragmentation_budget = 0.05f;      ///< Maximum tolerated waste ratio
                                             ///< before compaction.
    float compaction_threshold = 0.25f;      ///< Slab utilization threshold for
                                             ///< compaction.
    uint64_t compaction_interval_ms = 60000; ///< Minimum interval between
                                             ///< compaction cycles.
};

/// \brief Manages slab compaction across all caches.
///
/// Tracks fragmentation and triggers compaction when the waste budget exceeds a
/// configurable threshold (default 5%).
///
/// \note Methods that check \c SlabCache state must be called while holding
///       the owning thread's allocator lock or from a quiescent period.
class CompactionManager {
  public:
    /// \brief Construct with an optional configuration.
    ///
    /// \param[in] cfg Compaction parameters. Uses defaults when
    /// default-constructed.
    explicit CompactionManager(const CompactionConfig& cfg = {})
        : config_(cfg) {}

    /// \brief Check whether a slab should be compacted based on its
    /// utilization.
    ///
    /// \param[in] live_blocks Number of currently live blocks.
    /// \param[in] total_blocks Total blocks in the slab.
    /// \return \c true if utilization is below the compaction threshold.
    bool should_compact_slab(uint32_t live_blocks,
                             uint32_t total_blocks) const noexcept;

    /// \brief Result of a waste computation across a slab cache.
    struct WasteReport {
        size_t wasted_bytes{0};  ///< Bytes occupied by freed-but-unrecycled
                                 ///< blocks.
        size_t total_bytes{0};   ///< Total bytes across all slabs in the cache.
        float waste_ratio{0.0f}; ///< wasted_bytes / total_bytes, or 0 if no
                                 ///< slabs.
    };

    /// \brief Compute total fragmentation waste across a SlabCache.
    ///
    /// \param[in] cache The slab cache to analyze.
    /// \return A \c WasteReport with waste metrics.
    static WasteReport compute_waste(const SlabCache& cache) noexcept;

    /// \brief Check whether a compaction cycle should run now.
    ///
    /// \return \c true if the minimum interval has elapsed since the last
    /// cycle.
    bool should_compact() const noexcept;

    /// \brief Record that a compaction cycle ran (updates the last-compaction
    /// timestamp).
    void record_compaction() noexcept;

    /// \brief Return the current compaction configuration.
    ///
    /// \return A const reference to the config.
    const CompactionConfig& config() const noexcept {
        return config_;
    }

  private:
    CompactionConfig config_;
    int64_t last_compaction_ts_{0};
};

} // namespace hpactor::mem
