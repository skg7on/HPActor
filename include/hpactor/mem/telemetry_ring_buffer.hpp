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

/// \brief Types of allocation telemetry events recorded by the ring buffer.
enum class AllocationEventType : uint8_t {
    kAlloc = 0,        ///< A managed allocation was satisfied.
    kFree = 1,         ///< A managed block was freed.
    kCorruption = 2,   ///< Canary or guard-page corruption detected.
    kHibernateIn = 3,  ///< Actor state was serialized for hibernation.
    kHibernateOut = 4, ///< Actor state was deserialized from hibernation.
    kRejected = 5, ///< An allocation was rejected by region pressure admission.
};

/// \brief Compact (32-byte) allocation event stored in the telemetry ring
/// buffer.
struct AllocationEvent {
    uint64_t timestamp;  ///< Monotonic timestamp (rdtsc or steady_clock).
    uint32_t actor_id;   ///< Owning actor identifier.
    uint16_t block_size; ///< User bytes requested.
    uint8_t size_class;  ///< SizeClass index the request mapped to.
    uint8_t region_type; ///< RegionType the allocation was charged against.
    uint8_t event_type;  ///< AllocationEventType value.
    uint8_t _pad[7];     ///< Padding to 32 B alignment.
};

/// \brief Alias for the MPSC ring buffer specialized for allocation events.
///
/// Delegates to the shared \c adt::MpscRingBuffer ADT.
///
/// \tparam Capacity Ring buffer capacity in number of events (default 65536).
template <size_t Capacity = 65536>
using TelemetryRingBuffer = adt::MpscRingBuffer<AllocationEvent, Capacity>;

} // namespace hpactor::mem
