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

namespace hpactor::mem {

enum class RegionType : uint8_t {
    kActor     = 0,
    kMessage   = 1,
    kCoroutine = 2,
    kNetwork   = 3,
    kInternal  = 4,
    kHibernate = 5,
};

inline constexpr uint8_t kNumRegionTypes = 6;

// Per-region statistics (cache-line aligned to avoid false sharing)
struct alignas(64) RegionStats {
    std::atomic<uint64_t> total_allocated{0};
    std::atomic<uint64_t> total_freed{0};
    std::atomic<uint64_t> active_bytes{0};
    std::atomic<uint64_t> high_water_mark{0};
    std::atomic<uint64_t> alloc_count{0};
    std::atomic<uint64_t> free_count{0};
    std::atomic<uint64_t> corruption_events{0};
};

} // namespace hpactor::mem
