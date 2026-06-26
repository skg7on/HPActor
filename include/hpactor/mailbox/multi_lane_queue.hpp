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

#include <hpactor/mailbox/mpsc_mailbox.hpp>
#include <hpactor/mem/memory_config.hpp>

#include <cstdint>

namespace hpactor::mailbox {

/// Multi-lane MPSC queue container for priority-aware actor mailboxes.
///
/// Owns one system lane + N user lanes. Producers enqueue lock-free into
/// a specific lane. The consumer drains in fixed priority order:
/// system lane first, then user lanes 0..N-1.
///
/// Does NOT own the consumer lock — the caller (MPSCActorMailbox)
/// serializes dequeue/eviction externally. Does NOT know about
/// reservation, pressure, overflow policy, metrics, or scheduler wakeup.
///
/// \tparam T Message type with `std::atomic<T*> mpsc_next` member.
template <typename T> class MultiLaneQueue {
  public:
    static constexpr uint8_t kSystemLaneSentinel = 0xFF;
    static constexpr uint8_t kMaxUserLanes = 8;

    /// \brief Construct a multi-lane queue with the given number of user lanes.
    ///
    /// \param[in] num_user_lanes Number of user priority lanes (clamped to
    /// 1–8).
    explicit MultiLaneQueue(uint8_t num_user_lanes = 1)
        : num_user_lanes_(num_user_lanes) {}

    // ── Producer (lock-free, multi-producer safe) ─────────────────

    /// \brief Enqueue a message node into a specific lane (lock-free).
    ///
    /// \param[in] node Heap-allocated message node. Ownership transfers to
    ///                 the queue.
    /// \param[in] lane_idx Target lane: \c kSystemLaneSentinel for the system
    ///                     lane, or 0..N-1 for a user lane.
    /// \note Thread safety: lock-free — safe to call from any thread.
    ///       Multiple producers may enqueue concurrently.
    void enqueue(T* node, uint8_t lane_idx) noexcept {
        if (lane_idx == kSystemLaneSentinel) {
            system_lane_.enqueue(node);
        } else {
            user_lanes_[lane_idx].enqueue(node);
        }
    }

    // ── Consumer (NOT internally locked — caller serializes) ──────

    /// \brief Dequeue in priority order: system lane, then user lanes 0..N-1.
    ///
    /// \return Pointer to the dequeued node, or \c nullptr if all lanes are
    ///         empty. The caller owns the returned node and must destroy and
    ///         deallocate it.
    /// \note Thread safety: NOT internally locked. The caller
    ///       (\c MPSCActorMailbox) must serialize dequeue via its consumer
    ///       spin-lock. Safe to call concurrently with producers.
    T* dequeue() noexcept {
        T* node = system_lane_.dequeue();
        if (node)
            return node;
        for (uint8_t i = 0; i < num_user_lanes_; ++i) {
            node = user_lanes_[i].dequeue();
            if (node)
                return node;
        }
        return nullptr;
    }

    /// \brief Drop one message from the highest-priority non-empty user lane.
    ///
    /// Scans lanes 0..N-1 to find the globally oldest message. Within each
    /// lane, MPSC preserves FIFO, and lower lane index = higher priority =
    /// typically enqueued first since high-priority messages are dispatched
    /// to low-index lanes. Does NOT touch the system lane.
    ///
    /// \return Pointer to the dropped node, or \c nullptr if all user lanes
    ///         were empty. The caller must release the reservation and handle
    ///         deferred destruction.
    /// \note Thread safety: NOT internally locked — caller must serialize.
    T* try_drop_oldest_user_lane() noexcept {
        for (uint8_t i = 0; i < num_user_lanes_; ++i) {
            T* node = user_lanes_[i].dequeue();
            if (node)
                return node;
        }
        return nullptr;
    }

    /// Drop one message from the lowest-priority non-empty user lane.
    /// Does NOT touch the system lane.
    /// \return The dropped node (caller handles reservation release
    ///         and deferred destruction), or nullptr if all empty.
    T* try_drop_from_lowest_user_lane() noexcept {
        for (int i = static_cast<int>(num_user_lanes_) - 1; i >= 0; --i) {
            T* node = user_lanes_[i].dequeue();
            if (node)
                return node;
        }
        return nullptr;
    }

    // ── Pending free (deferred destructor) ────────────────────────

    /// \brief Number of deferred-free slots.
    ///
    /// Each slot delays destruction by one \c set_pending_free() call,
    /// giving producers that captured a node via \c head_.exchange()
    /// more time to complete \c prev->mpsc_next.store() before the node
    /// is freed.  A single slot (the old design) is prone to
    /// use-after-free under concurrent load when an OS-preempted
    /// producer outlasts two \c try_pop() cycles on the same mailbox.
    static constexpr uint8_t kPendingFreeRingSize = 8;

    /// \brief Stage a node for deferred destruction.
    ///
    /// Writes \p node into a ring buffer of \c kPendingFreeRingSize
    /// slots.  When the ring is full, the oldest entry is destroyed and
    /// deallocated — yielding a multi-cycle deferral window.  This
    /// prevents the use-after-free that occurs when a producer is
    /// preempted between \c head_.exchange() and
    /// \c prev->mpsc_next.store() and the consumer frees the captured
    /// node on the very next \c set_pending_free().
    ///
    /// \param[in] node Node to stage for deferred destruction. Ownership
    ///                 transfers to the queue.
    /// \note Thread safety: NOT internally locked — caller must serialize.
    /// \brief Optional recycling hook with context.  When set, freed nodes
    ///        are passed to this function instead of being deallocated.
    void (*recycle_hook_)(void* ctx, T*) = nullptr;
    void* recycle_ctx_ = nullptr;

    void set_pending_free(T* node) noexcept {
        if (pending_free_count_ == kPendingFreeRingSize) {
            T* oldest = pending_free_ring_[pending_free_write_idx_];
            if (recycle_hook_) {
                // Node stays live — move-assignment on reuse handles cleanup.
                recycle_hook_(recycle_ctx_, oldest);
            } else {
                oldest->~T();
                mem::deallocate(oldest);
            }
        } else {
            pending_free_count_++;
        }
        pending_free_ring_[pending_free_write_idx_] = node;
        pending_free_write_idx_ = static_cast<uint8_t>(
            (pending_free_write_idx_ + 1) % kPendingFreeRingSize);
    }

    /// \brief Drain and destroy all staged pending-free nodes.
    ///
    /// Iterates every occupied ring slot, calls the destructor, and
    /// deallocates the backing memory.  After this call the ring is
    /// empty.
    ///
    /// \note Thread safety: NOT internally locked — caller must serialize.
    /// \note The old single-slot API returned the staged pointer; this
    ///       multi-slot variant drains everything in one call.
    void drain_pending_free() noexcept {
        uint8_t count = pending_free_count_;
        // oldest entry is at (write_idx - count) mod ring_size
        uint8_t idx = static_cast<uint8_t>(
            (pending_free_write_idx_ + kPendingFreeRingSize - count) %
            kPendingFreeRingSize);
        for (uint8_t i = 0; i < count; ++i) {
            T* node = pending_free_ring_[idx];
            if (recycle_hook_) {
                recycle_hook_(recycle_ctx_, node);
            } else {
                node->~T();
                mem::deallocate(node);
            }
            idx = static_cast<uint8_t>((idx + 1) % kPendingFreeRingSize);
        }
        pending_free_count_ = 0;
        pending_free_write_idx_ = 0;
    }

    /// \brief Release the first staged pending-free node without
    ///        destroying it (used by the mailbox destructor).
    ///
    /// When the ring contains exactly one entry this behaves like the
    /// old single-slot API.  When the ring is empty returns \c nullptr.
    /// Callers that receive a non-null pointer own it and must destroy
    /// and deallocate it.
    ///
    /// \return Pointer to the oldest staged node, or \c nullptr if the
    ///         ring is empty.
    /// \note Thread safety: NOT internally locked — caller must serialize.
    T* release_pending_free() noexcept {
        if (pending_free_count_ == 0)
            return nullptr;
        uint8_t idx = static_cast<uint8_t>(
            (pending_free_write_idx_ + kPendingFreeRingSize - pending_free_count_) %
            kPendingFreeRingSize);
        T* p = pending_free_ring_[idx];
        // Shift remaining entries to keep the ring consistent for any
        // subsequent set_pending_free / drain_pending_free calls.
        pending_free_count_--;
        return p;
    }

    // ── Query ─────────────────────────────────────────────────────

    /// \brief Check whether all lanes are empty.
    ///
    /// \return \c true if the system lane and all user lanes are empty.
    /// \note Thread safety: lock-free — safe to call from any thread.
    ///       The result may be stale by the time the caller observes it.
    bool empty() const noexcept {
        if (!system_lane_.empty())
            return false;
        for (uint8_t i = 0; i < num_user_lanes_; ++i)
            if (!user_lanes_[i].empty())
                return false;
        return true;
    }

    /// \brief Total depth across all lanes (system + user).
    ///
    /// \return Sum of message counts in all lanes.
    /// \note Thread safety: lock-free — safe to call from any thread.
    int64_t total_depth() const noexcept {
        int64_t d = system_lane_.count();
        for (uint8_t i = 0; i < num_user_lanes_; ++i)
            d += user_lanes_[i].count();
        return d;
    }

    /// \brief Depth of a single lane.
    ///
    /// \param[in] lane_idx Lane index (0..N-1) or \c kSystemLaneSentinel.
    /// \return Message count in the specified lane.
    /// \note Thread safety: lock-free — safe to call from any thread.
    int64_t lane_depth(uint8_t lane_idx) const noexcept {
        if (lane_idx == kSystemLaneSentinel)
            return system_lane_.count();
        return user_lanes_[lane_idx].count();
    }

    /// \brief Current number of user lanes.
    ///
    /// \return Number of active user lanes (1–8).
    uint8_t num_user_lanes() const noexcept {
        return num_user_lanes_;
    }

    /// \brief Reconfigure the number of user lanes at runtime.
    ///
    /// \param[in] n New lane count (clamped to 1–8).
    void set_num_user_lanes(uint8_t n) {
        num_user_lanes_ = (n >= 1 && n <= kMaxUserLanes) ? n : 1;
    }

    // ── Test support ──────────────────────────────────────────────

    /// \brief Inject a message without scheduler notification (test-only).
    ///
    /// Delegates to \c enqueue() with the same lane routing.
    ///
    /// \param[in] node Heap-allocated message node.
    /// \param[in] lane_idx Target lane index.
    /// \note This is a test-only API. Do not use in production code paths.
    void inject_for_test(T* node, uint8_t lane_idx) noexcept {
        enqueue(node, lane_idx);
    }

    /// \brief Drain all lanes and destroy staged free nodes.
    ///
    /// Dequeues every message without destroying them (they are leaked —
    /// the caller must have already drained them), destroys all
    /// pending-free ring entries, and resets the lane count to 1.
    ///
    /// \note Thread safety: NOT internally locked — caller must ensure no
    ///       concurrent access.
    void reset() noexcept {
        while (dequeue() != nullptr) {
        }
        drain_pending_free();
        num_user_lanes_ = 1;
    }

  private:
    MPSCMailbox<T> system_lane_;
    MPSCMailbox<T> user_lanes_[kMaxUserLanes];
    uint8_t num_user_lanes_{1};

    // Deferred-free ring buffer: nodes survive kPendingFreeRingSize
    // set_pending_free() calls before being destroyed.  This gives
    // preempted producers enough time to complete their mpsc_next
    // stores, preventing the use-after-free that corrupts the MPSC
    // chain and causes the consumer to spin forever.
    T* pending_free_ring_[kPendingFreeRingSize]{};
    uint8_t pending_free_write_idx_{0};
    uint8_t pending_free_count_{0};
};

} // namespace hpactor::mailbox
