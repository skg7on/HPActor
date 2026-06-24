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
class TimerCommandQueue {
  public:
    static constexpr size_t kCapacity = 256;

    /// Push a command.  Returns false if the queue is full.
    /// Safe from any thread.
    bool try_push(TimerCommand cmd) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) % kCapacity;
        if (next == head_.load(std::memory_order_acquire))
            return false;
        buffer_[tail] = cmd;
        tail_.store(next, std::memory_order_release);
        return true;
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
