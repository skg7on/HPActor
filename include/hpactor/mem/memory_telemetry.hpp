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

/// \brief Singleton that records sampled allocation events into a lock-free
/// ring buffer for observability.
///
/// Sampling is controlled by \c sample_rate: one out of every N events is
/// recorded. Callers drain the ring buffer via \c drain() to export telemetry.
///
/// \note The ring buffer is MPSC (multiple producers via \c record_alloc /
///       \c record_free, single consumer via \c drain()). Safe to produce
///       from any thread.
class MemoryTelemetry {
  public:
    /// \brief Default sample rate: 1 out of every 128 allocations is recorded.
    static constexpr uint32_t kDefaultSampleRate = 128;

    /// \brief Return the singleton instance.
    static MemoryTelemetry& instance();

    /// \brief Set the telemetry sample rate.
    ///
    /// \param[in] sample_rate Record every Nth event (1 = record all).
    void set_sample_rate(uint32_t sample_rate) noexcept;

    /// \brief Return the current sample rate.
    ///
    /// \return Events are sampled at a rate of 1 / return_value.
    uint32_t sample_rate() const noexcept;

    /// \brief Record an allocation event (subject to sampling).
    ///
    /// \param[in] actor Owning actor.
    /// \param[in] region Memory region charged.
    /// \param[in] sc Size class of the allocation.
    /// \param[in] charged_bytes Number of bytes charged to the region.
    void record_alloc(ActorId actor, RegionType region, SizeClass sc,
                      size_t charged_bytes) noexcept;

    /// \brief Record a deallocation event (subject to sampling).
    ///
    /// \param[in] actor Owning actor.
    /// \param[in] region Memory region credited.
    /// \param[in] sc Size class of the freed block.
    /// \param[in] charged_bytes Number of bytes credited to the region.
    void record_free(ActorId actor, RegionType region, SizeClass sc,
                     size_t charged_bytes) noexcept;

    /// \brief Drain the ring buffer, invoking \p callback for each event.
    ///
    /// The callback is invoked sequentially for every event currently in the
    /// buffer. Events produced concurrently during the drain may or may not
    /// be included.
    ///
    /// \tparam F Callable with signature \c void(const AllocationEvent&).
    /// \param[in] callback Invoked for each drained event.
    /// \note Single-consumer: only one thread should call \c drain() at a time.
    template <typename F> void drain(F&& callback) {
        ring_.drain(std::forward<F>(callback));
    }

  private:
    /// \brief Internal record method that applies sampling and writes the
    /// event.
    void record(ActorId actor, RegionType region, SizeClass sc,
                size_t charged_bytes, AllocationEventType type) noexcept;

    TelemetryRingBuffer<> ring_{};
    std::atomic<uint32_t> sample_rate_{kDefaultSampleRate};
    std::atomic<uint64_t> sequence_{0};
};

} // namespace hpactor::mem
