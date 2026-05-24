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

#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/telemetry_ring_buffer.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <cstddef>

namespace hpactor::mem {

class MemoryTelemetry {
  public:
    static constexpr uint32_t kDefaultSampleRate = 128;

    static MemoryTelemetry& instance();

    void set_sample_rate(uint32_t sample_rate) noexcept;
    uint32_t sample_rate() const noexcept;

    void record_alloc(ActorId actor, RegionType region, SizeClass sc,
                      size_t charged_bytes) noexcept;
    void record_free(ActorId actor, RegionType region, SizeClass sc,
                     size_t charged_bytes) noexcept;

    template <typename F> void drain(F&& callback) {
        ring_.drain(std::forward<F>(callback));
    }

  private:
    void record(ActorId actor, RegionType region, SizeClass sc,
                size_t charged_bytes, AllocationEventType type) noexcept;

    TelemetryRingBuffer<> ring_{};
    std::atomic<uint32_t> sample_rate_{kDefaultSampleRate};
    std::atomic<uint64_t> sequence_{0};
};

} // namespace hpactor::mem