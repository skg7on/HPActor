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
#include <cstddef>
#include <cstdint>

namespace hpactor::mem {

// Allocation event for telemetry. Compact (32 bytes) for ring buffer density.
struct AllocationEvent {
    uint64_t timestamp;   // rdtsc or monotonic ns
    uint32_t actor_id;    // owning actor
    uint16_t block_size;  // user bytes requested
    uint8_t  size_class;  // SizeClass index
    uint8_t  region_type; // RegionType
    uint8_t  event_type;  // 0=alloc, 1=free, 2=corruption, 3=hibernate_in, 4=hibernate_out
    uint8_t  _pad[7];     // align to 32B
};

// Lock-free multi-producer single-consumer ring buffer.
// Producers (any thread): try_push() — non-blocking, returns false if full.
// Consumer (telemetry thread): drain() — reads all events since last drain.
template <size_t Capacity = 65536>
class TelemetryRingBuffer {
  public:
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

    // Non-blocking push from any thread. Uses CAS to reserve a slot.
    // Returns false if buffer is full.
    bool try_push(const AllocationEvent& event) noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t r = read_cursor_.load(std::memory_order_acquire);
            if (w - r >= Capacity) {
                return false; // full
            }
            if (write_cursor_.compare_exchange_weak(w, w + 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                buffer_[w & mask_] = event;
                return true;
            }
            // CAS failed — another producer claimed slot, w reloaded, retry
        }
    }

    // Drain all pending events. Call from single consumer thread.
    template <typename F>
    void drain(F&& callback) {
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        uint64_t w = write_cursor_.load(std::memory_order_acquire);

        while (r < w) {
            callback(buffer_[r & mask_]);
            ++r;
        }

        read_cursor_.store(r, std::memory_order_release);
    }

    // Number of pending events
    uint64_t available() const noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        uint64_t r = read_cursor_.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : 0;
    }

    bool empty() const noexcept { return available() == 0; }
    bool full() const noexcept { return available() >= Capacity; }

  private:
    static constexpr size_t mask_ = Capacity - 1;

    alignas(64) std::atomic<uint64_t> write_cursor_{0};
    alignas(64) std::atomic<uint64_t> read_cursor_{0};
    AllocationEvent buffer_[Capacity];
};

} // namespace hpactor::mem
