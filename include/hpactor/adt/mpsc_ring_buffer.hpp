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
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace hpactor::adt {

// =============================================================================
// MpscRingBuffer<T, Capacity> — MPSC ring buffer with compile-time capacity.
//
// Multi-producer, single-consumer. Capacity must be a power of two.
//
// Concurrency protocol (per-slot publish/sequence):
//   Each slot i has a sequence number seq_[i], initialized to i.
//   Producer: fetch_add write_cursor_ → write payload → seq_[slot].store(w+1,
//   release) Consumer: wait for seq_[slot].load(acquire) == r+1 → read →
//   seq_[slot].store(r+Cap, release)
//
// This closes the race where a consumer sees an advanced write cursor before
// the producer has written the payload.
// =============================================================================
template <typename T, size_t Capacity = 65536> class MpscRingBuffer {
    static_assert(Capacity >= 2, "Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power "
                                                    "of two");

  public:
    static constexpr size_t kDefaultCapacity = Capacity;

    MpscRingBuffer() : buffer_(Capacity), seq_(Capacity) {
        // Initialize sequence numbers: slot i expects write number i.
        for (size_t i = 0; i < Capacity; ++i) {
            seq_[i].store(i, std::memory_order_relaxed);
        }
    }

    // Try to push a value. Returns false if the buffer is full.
    // Safe to call from any number of threads concurrently.
    bool try_push(const T& value) noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t slot = w & mask_;
            // Slot is writable when its sequence number equals w.
            // The acquire load synchronizes with the consumer's release store,
            // ensuring we see the slot as free only after the consumer has
            // finished reading the previous occupant.
            if (seq_[slot].load(std::memory_order_acquire) != w) {
                events_lost_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (write_cursor_.compare_exchange_weak(w, w + 1,
                                                    std::memory_order_relaxed)) {
                break;
            }
        }
        // w is now exclusively claimed by this producer.
        buffer_[w & mask_] = value;
        // Publish: the release store ensures the buffer_ write is visible
        // before the consumer observes the published sequence number.
        seq_[w & mask_].store(w + 1, std::memory_order_release);
        return true;
    }

    // Drain all published elements. Must be called from a single consumer
    // thread.
    template <typename Fn> size_t drain(Fn&& callback) {
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        size_t count = 0;
        for (;;) {
            uint64_t slot = r & mask_;
            // Slot is ready when its sequence number equals r + 1.
            // The acquire load synchronizes with the producer's release store,
            // ensuring the payload write is visible.
            if (seq_[slot].load(std::memory_order_acquire) != (r + 1)) {
                break;
            }
            callback(buffer_[slot]);
            // Mark slot free for reuse. The release store ensures buffer_ read
            // completes before a producer observes the freed slot.
            seq_[slot].store(r + Capacity, std::memory_order_release);
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
        uint64_t r = read_cursor_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    bool empty() const noexcept {
        return size() == 0;
    }

  private:
    static constexpr size_t mask_ = Capacity - 1;

    alignas(64) std::atomic<uint64_t> write_cursor_{0};
    alignas(64) std::atomic<uint64_t> read_cursor_{0};
    alignas(64) std::atomic<uint64_t> events_lost_{0};
    std::vector<T> buffer_;
    std::vector<std::atomic<uint64_t>> seq_;
};

// NOTE: DynamicMpscRingBuffer below uses the same per-slot publish/sequence
// protocol as MpscRingBuffer above. Keep the two implementations in sync.

// =============================================================================
// DynamicMpscRingBuffer<T> — MPSC ring buffer with runtime capacity.
//
// Same concurrency contract as MpscRingBuffer but capacity is specified at
// construction time. Uses std::unique_ptr<T[]> for storage since the capacity
// is not a compile-time constant.
// =============================================================================
template <typename T> class DynamicMpscRingBuffer {
  public:
    // capacity must be > 0 and a power of two.
    explicit DynamicMpscRingBuffer(size_t capacity) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            std::fprintf(stderr,
                         "DynamicMpscRingBuffer: capacity must be a power of "
                         "two, got %zu\n",
                         capacity);
            std::abort();
        }
        capacity_ = capacity;
        mask_ = capacity - 1;
        buffer_ = std::make_unique<T[]>(capacity);
        seq_ = std::make_unique<std::atomic<uint64_t>[]>(capacity);
        for (size_t i = 0; i < capacity; ++i) {
            seq_[i].store(i, std::memory_order_relaxed);
        }
    }

    bool try_push(const T& value) noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t slot = w & mask_;
            if (seq_[slot].load(std::memory_order_acquire) != w) {
                events_lost_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (write_cursor_.compare_exchange_weak(w, w + 1,
                                                    std::memory_order_relaxed)) {
                break;
            }
        }
        buffer_[w & mask_] = value;
        seq_[w & mask_].store(w + 1, std::memory_order_release);
        return true;
    }

    template <typename Fn> size_t drain(Fn&& callback) {
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        size_t count = 0;
        for (;;) {
            uint64_t slot = r & mask_;
            if (seq_[slot].load(std::memory_order_acquire) != (r + 1)) {
                break;
            }
            callback(buffer_[slot]);
            seq_[slot].store(r + capacity_, std::memory_order_release);
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
        uint64_t r = read_cursor_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    bool empty() const noexcept {
        return size() == 0;
    }

  private:
    size_t capacity_{0};
    size_t mask_{0};
    std::unique_ptr<T[]> buffer_;
    std::unique_ptr<std::atomic<uint64_t>[]> seq_;

    alignas(64) std::atomic<uint64_t> write_cursor_{0};
    alignas(64) std::atomic<uint64_t> read_cursor_{0};
    alignas(64) std::atomic<uint64_t> events_lost_{0};
};

} // namespace hpactor::adt
