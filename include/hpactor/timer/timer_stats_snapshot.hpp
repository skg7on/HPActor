// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

namespace hpactor::sched {

/// \brief Snapshot of timer statistics across all shards in the TimerPlane.
///
/// Collected by \c HybridScheduler::timer_snapshot() and exposed via
/// \c ActorSystem::timer_stats() for CLI and metrics use.
struct TimerStatsSnapshot {
    uint32_t num_shards{0};

    // Aggregate counters across all shards
    uint64_t total_pending{0};
    uint64_t total_scheduled{0};
    uint64_t total_fired{0};
    uint64_t total_cancelled{0};
    uint64_t total_late{0};
    uint64_t total_dropped{0};

    /// Earliest deadline across all shards, or INT64_MAX if no timers pending.
    int64_t next_deadline{INT64_MAX};

    /// \brief Per-shard detail for inspection.
    struct ShardStats {
        uint64_t pending{0};
        uint64_t cmd_queue_depth{0};
        uint64_t fired{0};
        uint64_t late{0};
        uint64_t dropped{0};
        int64_t min_deadline{INT64_MAX};
    };

    std::vector<ShardStats> shards;
};

} // namespace hpactor::sched
