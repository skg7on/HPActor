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

#include <hpactor/mem/memory_telemetry.hpp>

#include <chrono>

namespace hpactor::mem {

MemoryTelemetry& MemoryTelemetry::instance() {
    static MemoryTelemetry telemetry;
    return telemetry;
}

void MemoryTelemetry::set_sample_rate(uint32_t sample_rate) noexcept {
    sample_rate_.store(sample_rate, std::memory_order_relaxed);
}

uint32_t MemoryTelemetry::sample_rate() const noexcept {
    return sample_rate_.load(std::memory_order_relaxed);
}

void MemoryTelemetry::record_alloc(ActorId actor, RegionType region,
                                   SizeClass sc, size_t charged_bytes) noexcept {
    record(actor, region, sc, charged_bytes, AllocationEventType::kAlloc);
}

void MemoryTelemetry::record_free(ActorId actor, RegionType region,
                                  SizeClass sc, size_t charged_bytes) noexcept {
    record(actor, region, sc, charged_bytes, AllocationEventType::kFree);
}

void MemoryTelemetry::record(ActorId actor, RegionType region, SizeClass sc,
                             size_t charged_bytes,
                             AllocationEventType type) noexcept {
    const uint32_t rate = sample_rate_.load(std::memory_order_relaxed);
    if (rate == 0) {
        return;
    }
    const uint64_t seq = sequence_.fetch_add(1, std::memory_order_relaxed);
    if (seq % rate != 0) {
        return;
    }

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    AllocationEvent event{};
    event.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    event.actor_id = static_cast<uint32_t>(actor.value());
    event.block_size = static_cast<uint16_t>(charged_bytes);
    event.size_class = static_cast<uint8_t>(sc);
    event.region_type = static_cast<uint8_t>(region);
    event.event_type = static_cast<uint8_t>(type);

    ring_.try_push(event); // best-effort — drop if buffer is full
}

} // namespace hpactor::mem