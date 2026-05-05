// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hpactor {
namespace metrics {

// Lock-free MPSC ring buffer. Multi-producer, single-consumer.
// Capacity must be a power of two.
// Producer: reserve() CAS-claims a slot, caller writes fields directly.
// Consumer: drain(callback) reads all committed slots since last drain.
template <typename T, size_t Capacity = 65536>
class MpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    static constexpr size_t kDefaultCapacity = Capacity;

    MpscRingBuffer() : buffer_(Capacity) {}

    // Producer: atomically claim a slot. Returns nullptr if buffer is full.
    // Caller writes directly into the returned slot; no separate commit needed.
    T* reserve() noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_relaxed);
        do {
            if (w - read_cursor_.load(std::memory_order_acquire) >= Capacity) {
                events_lost_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
        } while (!write_cursor_.compare_exchange_weak(
            w, w + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
        return &buffer_[w & mask_];
    }

    // Consumer: drain all committed slots since last drain.
    template <typename Fn>
    size_t drain(Fn&& callback) {
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
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

} // namespace metrics
} // namespace hpactor
