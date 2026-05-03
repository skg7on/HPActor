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

#include <hpactor/mem/memory_tracker.hpp>

namespace hpactor::mem {

MemoryTracker& MemoryTracker::instance() {
    static MemoryTracker mt;
    return mt;
}

MemoryTracker::MemoryTracker()
    : stats_(new ActorMemoryStats[kMaxTrackedActors]),
      capacity_(kMaxTrackedActors) {}

size_t MemoryTracker::index_for(ActorId actor) const noexcept {
    return static_cast<size_t>(actor.value() % kMaxTrackedActors);
}

bool MemoryTracker::record_alloc(ActorId actor, size_t bytes) noexcept {
    size_t idx = index_for(actor);
    if (idx >= capacity_) return false;

    auto& s = stats_[idx];

    uint64_t prev = __atomic_fetch_add(&s.current_bytes, bytes, __ATOMIC_RELAXED);
    uint64_t curr = prev + bytes;

    // Update peak lazily via CAS
    uint64_t peak = __atomic_load_n(&s.peak_bytes, __ATOMIC_RELAXED);
    while (curr > peak) {
        if (__atomic_compare_exchange_n(&s.peak_bytes, &peak, curr,
                /*weak=*/false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            break;
        }
    }

    __atomic_fetch_add(&s.alloc_count, 1, __ATOMIC_RELAXED);
    return true;
}

void MemoryTracker::record_free(ActorId actor, size_t bytes) noexcept {
    size_t idx = index_for(actor);
    if (idx >= capacity_) return;

    auto& s = stats_[idx];
    __atomic_fetch_sub(&s.current_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s.free_count, 1, __ATOMIC_RELAXED);
}

void MemoryTracker::snapshot(ActorId actor, ActorMemoryStats& out) const noexcept {
    size_t idx = index_for(actor);
    if (idx >= capacity_) return;

    const auto& s = stats_[idx];
    out.current_bytes = __atomic_load_n(&s.current_bytes, __ATOMIC_RELAXED);
    out.peak_bytes    = __atomic_load_n(&s.peak_bytes, __ATOMIC_RELAXED);
    out.alloc_count   = __atomic_load_n(&s.alloc_count, __ATOMIC_RELAXED);
    out.free_count    = __atomic_load_n(&s.free_count, __ATOMIC_RELAXED);
    out.last_alloc_ns = __atomic_load_n(&s.last_alloc_ns, __ATOMIC_RELAXED);
}

uint64_t MemoryTracker::total_active_bytes() const noexcept {
    uint64_t total = 0;
    for (size_t i = 0; i < capacity_; ++i) {
        total += __atomic_load_n(&stats_[i].current_bytes, __ATOMIC_RELAXED);
    }
    return total;
}

uint64_t MemoryTracker::total_peak_bytes() const noexcept {
    uint64_t total = 0;
    for (size_t i = 0; i < capacity_; ++i) {
        total += __atomic_load_n(&stats_[i].peak_bytes, __ATOMIC_RELAXED);
    }
    return total;
}

uint64_t MemoryTracker::total_alloc_count() const noexcept {
    uint64_t total = 0;
    for (size_t i = 0; i < capacity_; ++i) {
        total += __atomic_load_n(&stats_[i].alloc_count, __ATOMIC_RELAXED);
    }
    return total;
}

} // namespace hpactor::mem
