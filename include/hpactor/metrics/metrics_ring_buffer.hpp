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
#include <vector>

namespace hpactor::metrics {

// Lock-free MPSC ring buffer. Multi-producer, single-consumer.
// Capacity must be a power of two.
// Producer: try_push(value) writes the slot then CAS-claims it with a release fence.
// Consumer: drain(callback) reads all committed slots since last drain.
template <typename T, size_t Capacity = 65536>
class MpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    static constexpr size_t kDefaultCapacity = Capacity;

    MpscRingBuffer() : buffer_(Capacity) {}

    // Producer: write the value, then CAS-claim the slot with a release fence
    // to ensure the write is visible before the consumer sees the increment.
    bool try_push(const T& value) noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_relaxed);
        do {
            if (w - read_cursor_.load(std::memory_order_acquire) >= Capacity) {
                events_lost_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        } while (!write_cursor_.compare_exchange_weak(
            w, w + 1, std::memory_order_acquire, std::memory_order_relaxed));
        buffer_[w & mask_] = value;
        std::atomic_thread_fence(std::memory_order_release);
        return true;
    }

    // Consumer: drain all committed slots since last drain.
    template <typename Fn>
    size_t drain(Fn&& callback) {
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_acquire);
        size_t count = 0;
        while (r < w) {
            callback(buffer_[r & mask_]);
            ++r;
            ++count;
        }
        read_cursor_.store(r, std::memory_order_release);
        return count;
    }

    uint64_t events_lost() const noexcept {
        return events_lost_.load(std::memory_order_relaxed);
    }

    size_t size() const noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        return static_cast<size_t>(w - r);
    }

    bool empty() const noexcept { return size() == 0; }

private:
    static constexpr size_t mask_ = Capacity - 1;
    alignas(64) std::atomic<uint64_t> write_cursor_{0};
    alignas(64) std::atomic<uint64_t> read_cursor_{0};
    alignas(64) std::atomic<uint64_t> events_lost_{0};
    std::vector<T> buffer_;
};

} // namespace hpactor::metrics
