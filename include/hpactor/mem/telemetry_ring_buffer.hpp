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

#include <cstdint>
#include <hpactor/adt/mpsc_ring_buffer.hpp>

namespace hpactor::mem {

// Allocation event for telemetry. Compact (32 bytes) for ring buffer density.
struct AllocationEvent {
    uint64_t timestamp;  // rdtsc or monotonic ns
    uint32_t actor_id;   // owning actor
    uint16_t block_size; // user bytes requested
    uint8_t size_class;  // SizeClass index
    uint8_t region_type; // RegionType
    uint8_t event_type;  // 0=alloc, 1=free, 2=corruption, 3=hibernate_in,
                         // 4=hibernate_out
    uint8_t _pad[7];     // align to 32B
};

// Alias for backward compatibility — delegates to the shared ADT.
template <size_t Capacity = 65536>
using TelemetryRingBuffer = adt::MpscRingBuffer<AllocationEvent, Capacity>;

} // namespace hpactor::mem
