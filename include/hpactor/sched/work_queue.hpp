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
            return buf[i & mask].load(std::memory_order_relaxed);
        }
        void put(int64_t i, T v) {
            buf[i & mask].store(v, std::memory_order_relaxed);
        }
        CircularArray* grow(int64_t bottom, int64_t top) const;
    };

    std::atomic<int64_t>        top_{0};      // shared: thieves read, CAS to increment
    std::atomic<int64_t>        bottom_{0};   // owned: only owner writes
    std::atomic<CircularArray*> array_;
    std::vector<CircularArray*> garbage_;     // old arrays pending reclamation
};

// MultiPriorityWorkQueue: array of P Chase-Lev deques, one per priority level.
// Priority 0 = highest. Operations are owner-only (push/pop); stealing is done
// directly on the target worker's deque via WorkerThread::try_steal().
class MultiPriorityWorkQueue {
public:
    explicit MultiPriorityWorkQueue(uint32_t priority_levels = 4);

    void push(uint8_t priority, WorkItem item);   // owner only
    bool pop(WorkItem& out);                     // scans high→low, owner only
    size_t depth_approx() const;                  // sum of all level sizes

    uint32_t num_levels() const { return static_cast<uint32_t>(levels_.size()); }

private:
    std::vector<ChaselevDeque<WorkItem>> levels_;
};

} // namespace hpactor::sched