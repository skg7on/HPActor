// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/sched/scheduler_interfaces.hpp>
#include <hpactor/timer/timer_command_queue.hpp>
#include <hpactor/timer/timer_node.hpp>
#include <hpactor/timer/timer_options.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace hpactor::sched {

/// \brief Per-worker-shard timer wheel with slot-array handle resolution.
///
/// Each shard is independently lockable (mutex per shard).  The owning
/// TimerPlane polls all shards from a single timer thread.
///
/// The wheel has 4 hierarchical levels with 256 buckets each, modelled
/// after the Linux kernel timer wheel.  A slot array provides O(1)
/// handle-to-node resolution for cancel, with generation-based ABA
/// protection on slot reuse.
///
/// Callbacks are collected under the mutex during advance() and fired
/// outside the lock so that callbacks may safely call schedule() or
/// cancel() without deadlock.
class TimerPlaneShard {
  public:
    static constexpr uint32_t kMaxSlots = 65536;
    static constexpr uint32_t kInvalidSlot = UINT32_MAX;
    static constexpr uint32_t kNumLevels = 4;
    static constexpr uint32_t kBucketsPerLevel = 256;

    /// \brief Construct a shard.
    /// \param[in] shard_index Index of this shard within the TimerPlane.
    /// \param[in] tick_ns Duration of one tick at level 0, in nanoseconds.
    explicit TimerPlaneShard(uint32_t shard_index, int64_t tick_ns = 1'000'000);

    ~TimerPlaneShard();

    TimerPlaneShard(const TimerPlaneShard&) = delete;
    TimerPlaneShard& operator=(const TimerPlaneShard&) = delete;
    TimerPlaneShard(TimerPlaneShard&&) = delete;
    TimerPlaneShard& operator=(TimerPlaneShard&&) = delete;

    /// \brief Schedule a timer. Returns encoded TimerHandle, or invalid
    ///        handle on failure (e.g. slot exhaustion).
    TimerHandle schedule(int64_t delay_ns, timer_callback cb,
                         uint64_t group_handle = 0, uint8_t priority = 0);

    /// \brief Cancel a timer by encoded handle. Returns true if cancelled.
    bool cancel(TimerHandle handle);

    /// \brief Advance time: drain command queue, fire expired timers.
    /// \return The number of timers fired.
    uint32_t advance(int64_t now_ns);

    /// \brief Push a cross-thread command. Returns false if queue is full.
    bool push_command(TimerCommand cmd);

    // Lock-free metric queries

    /// \brief Earliest timer deadline in nanoseconds, or INT64_MAX if empty.
    int64_t min_deadline_ns() const {
        return min_deadline_.load(std::memory_order_acquire);
    }

    uint64_t pending_count() const {
        return pending_.load(std::memory_order_relaxed);
    }
    uint64_t scheduled_count() const {
        return scheduled_.load(std::memory_order_relaxed);
    }
    uint64_t fired_count() const {
        return fired_.load(std::memory_order_relaxed);
    }
    uint64_t cancelled_count() const {
        return cancelled_.load(std::memory_order_relaxed);
    }
    uint64_t late_count() const {
        return late_.load(std::memory_order_relaxed);
    }
    uint64_t dropped_count() const {
        return dropped_.load(std::memory_order_relaxed);
    }

    /// \brief Approximate count of commands waiting in the cross-thread queue.
    size_t cmd_queue_depth() const {
        return cmd_queue_.size();
    }

    /// \brief Access the shard mutex for direct operations.
    std::mutex& mutex() {
        return mutex_;
    }

  private:
    /// \brief Doubly-linked list of timers within a single wheel bucket.
    struct BucketList {
        TimerNode* head{nullptr};
        TimerNode* tail{nullptr};
        uint32_t count{0};

        void push_back(TimerNode* node);
        void unlink(TimerNode* node);
    };

    /// \brief One level of the hierarchical timing wheel.
    struct WheelLevel {
        std::vector<BucketList> buckets; ///< kBucketsPerLevel buckets
        uint32_t mask;
    };

    /// \brief Insert a node into the wheel at the correct level and bucket.
    void insert_into_wheel(TimerNode* node);

    /// \brief Remove a node from its current bucket (O(1) unlink).
    void remove_from_wheel(TimerNode* node);

    /// \brief Acquire a free slot, store the node, increment generation.
    /// \return Slot index, or kMaxSlots if none available.
    uint32_t acquire_slot(TimerNode* node);

    /// \brief Release a slot back to the free list with ABA guard.
    void release_slot(uint32_t slot_index, uint8_t expected_generation);

    /// \brief Recompute min_deadline_ from all pending timers in the wheel.
    void recompute_min_deadline();

    uint32_t shard_index_;
    int64_t tick_ns_;
    int64_t current_time_ns_{0};

    /// Slot array for O(1) handle-to-node resolution.
    std::vector<TimerNode*> slots_;
    std::vector<uint8_t> generations_;
    std::vector<uint32_t> free_slots_; ///< LIFO free slot indices

    /// Hierarchical timing wheel.
    std::vector<WheelLevel> levels_;
    std::vector<int64_t> level_ranges_; ///< Precomputed max range per level

    /// Lock-free MPSC command queue for cross-thread operations.
    TimerCommandQueue cmd_queue_;

    /// Metrics (updated under mutex, read lock-free).
    std::atomic<int64_t> min_deadline_{INT64_MAX};
    std::atomic<uint64_t> pending_{0};
    std::atomic<uint64_t> scheduled_{0};
    std::atomic<uint64_t> fired_{0};
    std::atomic<uint64_t> cancelled_{0};
    std::atomic<uint64_t> late_{0};
    std::atomic<uint64_t> dropped_{0};

    /// \brief Serializes all wheel and slot mutations.
    mutable std::mutex mutex_;
};

} // namespace hpactor::sched
