// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/timer/timer_node.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace hpactor::sched {

/// Lock-free bounded MPSC command queue for cross-thread timer operations.
///
/// Single consumer (the shard's advance() loop), multiple producers
/// (any thread calling schedule_after / cancel_timer).
///
/// MPSC protocol: producers CAS-claim a tail slot atomically before writing.
/// Only the consumer thread calls drain_all().
class TimerCommandQueue {
  public:
    static constexpr size_t kCapacity = 256;

    /// Push a command.  Returns false if the queue is full.
    /// Safe for multiple concurrent producers (CAS-based slot claim).
    bool try_push(TimerCommand cmd) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        for (;;) {
            size_t next = (tail + 1) % kCapacity;
            if (next == head_.load(std::memory_order_acquire))
                return false; // full
            // Try to claim slot 'tail' atomically
            if (tail_.compare_exchange_weak(tail, next, std::memory_order_release,
                                            std::memory_order_relaxed)) {
                buffer_[tail] = cmd;
                return true;
            }
            // CAS failed -- another producer claimed this slot; retry with new
            // tail
        }
    }

    /// Approximate number of commands currently in the queue.
    /// Safe from any thread; may be stale by the time the caller acts on it.
    size_t size() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        if (tail >= head)
            return tail - head;
        return kCapacity - head + tail;
    }

    /// Drain all pending commands into `out`.  Returns the number drained.
    /// Must only be called by the owning shard thread.
    size_t drain_all(std::vector<TimerCommand>& out) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t count = 0;
        while (head != tail) {
            out.push_back(buffer_[head]);
            head = (head + 1) % kCapacity;
            ++count;
        }
        head_.store(head, std::memory_order_release);
        return count;
    }

  private:
    std::array<TimerCommand, kCapacity> buffer_{};
    std::atomic<size_t> head_{0}; // consumer index
    std::atomic<size_t> tail_{0}; // producer index
};

} // namespace hpactor::sched
