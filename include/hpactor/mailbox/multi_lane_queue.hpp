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
template <typename T>
class MultiLaneQueue {
public:
    static constexpr uint8_t kSystemLaneSentinel = 0xFF;
    static constexpr uint8_t kMaxUserLanes = 8;

    explicit MultiLaneQueue(uint8_t num_user_lanes = 1)
        : num_user_lanes_(num_user_lanes) {}

    // ── Producer (lock-free, multi-producer safe) ─────────────────

    void enqueue(T* node, uint8_t lane_idx) noexcept {
        if (lane_idx == kSystemLaneSentinel) {
            system_lane_.enqueue(node);
        } else {
            user_lanes_[lane_idx].enqueue(node);
        }
    }

    // ── Consumer (NOT internally locked — caller serializes) ──────

    /// Dequeue in priority order: system lane, then user lanes 0..N-1.
    /// Returns nullptr if all lanes are empty.
    T* dequeue() noexcept {
        T* node = system_lane_.dequeue();
        if (node) return node;
        for (uint8_t i = 0; i < num_user_lanes_; ++i) {
            node = user_lanes_[i].dequeue();
            if (node) return node;
        }
        return nullptr;
    }

    /// Drop one message from the highest-priority non-empty user lane.
    /// Scans lanes 0..N-1 to find the globally oldest message (within
    /// each lane, MPSC preserves FIFO, and lower lane index = higher
    /// priority = typically enqueued first since high-priority messages
    /// are dispatched to low-index lanes).
    /// Does NOT touch the system lane.
    T* try_drop_oldest_user_lane() noexcept {
        for (uint8_t i = 0; i < num_user_lanes_; ++i) {
            T* node = user_lanes_[i].dequeue();
            if (node) return node;
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
            if (node) return node;
        }
        return nullptr;
    }

    // ── Pending free (deferred destructor) ────────────────────────

    void set_pending_free(T* node) noexcept {
        if (pending_free_) {
            pending_free_->~T();
            mem::deallocate(pending_free_);
        }
        pending_free_ = node;
    }

    T* release_pending_free() noexcept {
        T* p = pending_free_;
        pending_free_ = nullptr;
        return p;
    }

    // ── Query ─────────────────────────────────────────────────────

    bool empty() const noexcept {
        if (!system_lane_.empty()) return false;
        for (uint8_t i = 0; i < num_user_lanes_; ++i)
            if (!user_lanes_[i].empty()) return false;
        return true;
    }

    int64_t total_depth() const noexcept {
        int64_t d = system_lane_.count();
        for (uint8_t i = 0; i < num_user_lanes_; ++i)
            d += user_lanes_[i].count();
        return d;
    }

    int64_t lane_depth(uint8_t lane_idx) const noexcept {
        if (lane_idx == kSystemLaneSentinel) return system_lane_.count();
        return user_lanes_[lane_idx].count();
    }

    uint8_t num_user_lanes() const noexcept {
        return num_user_lanes_;
    }

    void set_num_user_lanes(uint8_t n) {
        num_user_lanes_ = (n >= 1 && n <= kMaxUserLanes) ? n : 1;
    }

    // ── Test support ──────────────────────────────────────────────

    void inject_for_test(T* node, uint8_t lane_idx) noexcept {
        enqueue(node, lane_idx);
    }

    void reset() noexcept {
        while (dequeue() != nullptr) {}
        if (pending_free_) {
            pending_free_->~T();
            mem::deallocate(pending_free_);
            pending_free_ = nullptr;
        }
        num_user_lanes_ = 1;
    }

private:
    MPSCMailbox<T> system_lane_;
    MPSCMailbox<T> user_lanes_[kMaxUserLanes];
    uint8_t num_user_lanes_{1};
    T* pending_free_{nullptr};
};

} // namespace hpactor::mailbox
