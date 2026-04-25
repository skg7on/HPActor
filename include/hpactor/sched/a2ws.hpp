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
#include <cstdint>
#include <vector>

namespace hpactor::sched {

// -----------------------------------------------------------------------------
// A2WS: Adaptive Two-Level Work Stealing
// -----------------------------------------------------------------------------
// Implements adaptive work-stealing with two levels:
// 1. Local level: try workers in same NUMA/pool region first
// 2. Global level: if local pool is empty, spread to other pools
//
// Each worker tracks:
// - victim_index: current victim suggestion
// - steal_attempts: number of steal attempts
// - steal_successes: number of successful steals
//
// The adaptive aspect: if a victim consistently fails (empty queue),
// the thief will switch victims. Success rate determines victim selection.
// -----------------------------------------------------------------------------
class A2WS {
  public:
    struct WorkerStats {
        std::atomic<uint64_t> steal_attempts{0};
        std::atomic<uint64_t> steal_successes{0};
        std::atomic<uint64_t> local_steals{0};  // stolen from local pool
        std::atomic<uint64_t> remote_steals{0}; // stolen from remote pool
    };

    A2WS() = default;

    explicit A2WS(uint32_t num_workers, uint32_t pool_size = 4);

    // Get next victim to try stealing from
    // Returns worker index, wraps around via round-robin
    uint32_t get_victim(uint32_t thief_index);

    // Record a steal attempt (successful or not)
    void record_attempt(uint32_t thief, uint32_t victim, bool success);

    // Record successful steal from victim
    void record_steal(uint32_t thief, uint32_t victim);

    // Get victim pool range for a given worker
    // Returns [pool_start, pool_end) exclusive
    void get_victim_pool(uint32_t worker, uint32_t& start, uint32_t& end) const;

    // Number of workers
    uint32_t num_workers() const {
        return num_workers_;
    }

    // Check if two workers are in the same local pool
    bool same_pool(uint32_t a, uint32_t b) const;

    // Stats access
    const WorkerStats& stats(uint32_t worker) const {
        return workers_[worker];
    }

  private:
    uint32_t num_workers_;
    uint32_t pool_size_; // workers per local pool
    std::vector<WorkerStats> workers_;
    std::vector<std::atomic<uint32_t>> victim_hints_; // per-thief victim
                                                      // suggestion
};

} // namespace hpactor::sched