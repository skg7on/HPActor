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

// include/hpactor/sched/work_queue.hpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <algorithm>

#include <hpactor/types/types.hpp>

namespace hpactor::sched {

// WorkItem: unit of scheduling work enqueued to a worker deque
struct WorkItem {
    ActorId  actor;
    int64_t  deadline_ns;   // absolute CLOCK_MONOTONIC; INT64_MAX if none
    uint64_t sequence;      // FIFO tiebreaker within same deadline
};

// Chase-Lev lock-free work-stealing deque.
// T must be default-constructible and copyable.
template<typename T>
class ChaselevDeque {
public:
    explicit ChaselevDeque(size_t initial_capacity = 256);
    ~ChaselevDeque();

    // Non-copyable, non-movable
    ChaselevDeque(const ChaselevDeque&) = delete;
    ChaselevDeque& operator=(const ChaselevDeque&) = delete;
    ChaselevDeque(ChaselevDeque&&) = delete;
    ChaselevDeque& operator=(ChaselevDeque&&) = delete;

    // Owner operations — wait-free O(1) amortized
    void push_bottom(T item);
    bool pop_bottom(T& out);

    // Thief operation — lock-free O(1), ABA-safe via version tag
    bool steal_top(T& out);

    // Approximate size (relaxed read, for diagnostics/load estimation)
    size_t size_approx() const;

private:
    struct CircularArray {
        std::vector<std::atomic<T>> buf;
        size_t                      mask;  // capacity - 1 (power-of-2)
        explicit CircularArray(size_t cap);

        T get(int64_t i) const {
            return buf[static_cast<size_t>(i) & mask].load(std::memory_order_relaxed);
        }
        void put(int64_t i, T v) {
            buf[static_cast<size_t>(i) & mask].store(v, std::memory_order_relaxed);
        }
        CircularArray* grow(int64_t bottom, int64_t top) const;
    };

    std::atomic<int64_t>        top_{0};      // shared: thieves read, CAS to increment
    std::atomic<int64_t>        bottom_{0};   // owned: only owner writes
    std::atomic<CircularArray*> array_;
    std::vector<CircularArray*> garbage_;     // old arrays pending reclamation
};

// --- CircularArray (nested in ChaselevDeque) ---
template<typename T>
ChaselevDeque<T>::CircularArray::CircularArray(size_t cap) : buf(cap), mask(cap - 1) {
    for (auto& slot : buf) {
        slot.store(T{}, std::memory_order_relaxed);
    }
}

template<typename T>
typename ChaselevDeque<T>::CircularArray*
ChaselevDeque<T>::CircularArray::grow(int64_t bottom, int64_t top) const {
    size_t new_cap = buf.size() * 2;
    auto* new_arr = new CircularArray(new_cap);
    for (int64_t i = top; i < bottom; ++i) {
        new_arr->put(i, get(i));
    }
    return new_arr;
}

// --- ChaselevDeque ---
template<typename T>
ChaselevDeque<T>::ChaselevDeque(size_t initial_capacity)
    : array_(new CircularArray(initial_capacity)) {}

template<typename T>
ChaselevDeque<T>::~ChaselevDeque() {
    delete array_.load(std::memory_order_acquire);
    for (auto* arr : garbage_) {
        delete arr;
    }
}

template<typename T>
void ChaselevDeque<T>::push_bottom(T item) {
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_acquire);
    auto* arr = array_.load(std::memory_order_acquire);

    if (b - t > static_cast<int64_t>(arr->mask)) {
        auto* new_arr = arr->grow(b, t);
        garbage_.push_back(arr);
        array_.store(new_arr, std::memory_order_release);
        arr = new_arr;
    }

    arr->put(b, std::move(item));
    bottom_.store(b + 1, std::memory_order_release);
}

template<typename T>
bool ChaselevDeque<T>::pop_bottom(T& out) {
    int64_t b = bottom_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    int64_t t = top_.load(std::memory_order_acquire);

    auto* arr = array_.load(std::memory_order_acquire);
    if (b - t < 0) {
        // Empty or underflow — restore bottom to original value
        bottom_.store(t, std::memory_order_release);
        return false;
    }

    out = arr->get(b);

    if (b == t) {
        // Only one item — try to CAS to prevent thief from stealing
        if (!top_.compare_exchange_strong(t, t + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            // Lost CAS — thief got the item, restore bottom to match
            bottom_.store(t, std::memory_order_release);
            return false;
        }
        // Won CAS — owner claimed the last item
        return true;
    }

    return true;
}

template<typename T>
bool ChaselevDeque<T>::steal_top(T& out) {
    int64_t t = top_.load(std::memory_order_acquire);
    int64_t b = bottom_.load(std::memory_order_acquire);

    if (b - t <= 0) {
        return false;
    }

    auto* arr = array_.load(std::memory_order_acquire);
    out = arr->get(t);

    if (!top_.compare_exchange_strong(t, t + 1,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
        return false;  // ABA race
    }

    return true;
}

template<typename T>
size_t ChaselevDeque<T>::size_approx() const {
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_relaxed);
    return static_cast<size_t>(b >= t ? b - t : 0);
}

// MultiPriorityWorkQueue: array of P Chase-Lev deques, one per priority level.
// Priority 0 = highest. Operations are owner-only (push/pop); stealing is done
// directly on the target worker's deque via WorkerThread::try_steal().
class MultiPriorityWorkQueue {
public:
    explicit MultiPriorityWorkQueue(uint32_t priority_levels = 4) : levels_(priority_levels) {}

    void push(uint8_t priority, WorkItem item) {
        levels_[priority].push_bottom(item);
    }

    bool pop(WorkItem& out) {
        // Scan from highest priority to lowest
        for (uint32_t i = 0; i < levels_.size(); ++i) {
            if (levels_[i].pop_bottom(out)) {
                return true;
            }
        }
        return false;
    }

    size_t depth_approx() const {
        size_t total = 0;
        for (const auto& level : levels_) {
            total += level.size_approx();
        }
        return total;
    }

    uint32_t num_levels() const { return static_cast<uint32_t>(levels_.size()); }

private:
    std::vector<ChaselevDeque<WorkItem>> levels_;
};

} // namespace hpactor::sched
