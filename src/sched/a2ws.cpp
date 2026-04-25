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

#include <hpactor/sched/a2ws.hpp>

namespace hpactor::sched {

A2WS::A2WS(uint32_t num_workers, uint32_t pool_size)
    : num_workers_(num_workers), pool_size_(pool_size), workers_(num_workers),
      victim_hints_(num_workers) {
    // Initialize victim hints to point to self + 1 (start scanning from next
    // worker)
    for (uint32_t i = 0; i < num_workers; ++i) {
        victim_hints_[i].store((i + 1) % num_workers, std::memory_order_relaxed);
    }
}

uint32_t A2WS::get_victim(uint32_t thief_index) {
    // Simple round-robin with hint
    uint32_t hint = victim_hints_[thief_index].load(std::memory_order_relaxed);
    return hint;
}

void A2WS::record_attempt(uint32_t thief, uint32_t victim, bool success) {
    workers_[thief].steal_attempts.fetch_add(1, std::memory_order_relaxed);

    if (success) {
        workers_[thief].steal_successes.fetch_add(1, std::memory_order_relaxed);
    }

    // Update victim hint based on result
    // If steal failed, advance to next potential victim
    uint32_t current_hint = victim_hints_[thief].load(std::memory_order_relaxed);
    uint32_t next_hint = (victim + 1) % num_workers_;

    if (!success) {
        // Failed steal - try next victim
        victim_hints_[thief].store(next_hint, std::memory_order_relaxed);
    } else {
        // Success - stick with current victim for now
        // Could also use adaptive logic to prefer victims with history of
        // success
    }

    (void)thief;        // Currently unused in victim selection
    (void)current_hint; // Currently unused
}

void A2WS::record_steal(uint32_t thief, uint32_t victim) {
    if (same_pool(thief, victim)) {
        workers_[thief].local_steals.fetch_add(1, std::memory_order_relaxed);
    } else {
        workers_[thief].remote_steals.fetch_add(1, std::memory_order_relaxed);
    }
}

void A2WS::get_victim_pool(uint32_t worker, uint32_t& start, uint32_t& end) const {
    uint32_t pool_id = worker / pool_size_;
    start = pool_id * pool_size_;
    end = std::min(start + pool_size_, num_workers_);
}

bool A2WS::same_pool(uint32_t a, uint32_t b) const {
    return (a / pool_size_) == (b / pool_size_);
}

} // namespace hpactor::sched