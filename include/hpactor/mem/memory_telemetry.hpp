// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

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
