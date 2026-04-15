#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <algorithm>

#include <hpactor/types/types.hpp>

namespace hpactor::sched {

struct WorkItem {
    ActorId  actor;
    int64_t  deadline_ns;
    uint64_t sequence;
};

template<typename T>
class ChaselevDeque {
public:
    explicit ChaselevDeque(size_t initial_capacity = 256);
    ~ChaselevDeque();

    ChaselevDeque(const ChaselevDeque&) = delete;
    ChaselevDeque& operator=(const ChaselevDeque&) = delete;
    ChaselevDeque(ChaselevDeque&&) = delete;
    ChaselevDeque& operator=(ChaselevDeque&&) = delete;

    void push_bottom(T item);
    bool pop_bottom(T& out);
    bool steal_top(T& out);
    size_t size_approx() const;

private:
    struct CircularArray {
        std::vector<std::atomic<T>> buf;
        size_t                      mask;
        explicit CircularArray(size_t cap);

        T get(int64_t i) const {
            return buf[static_cast<size_t>(i) & mask].load(std::memory_order_relaxed);
        }
        void put(int64_t i, T v) {
            buf[static_cast<size_t>(i) & mask].store(v, std::memory_order_relaxed);
        }
        CircularArray* grow(int64_t bottom, int64_t top) const;
    };

    std::atomic<int64_t>        top_{0};
    std::atomic<int64_t>        bottom_{0};
    std::atomic<CircularArray*> array_;
    std::vector<CircularArray*> garbage_;
};

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
        bottom_.store(b + 1, std::memory_order_release);
        return false;
    }

    out = arr->get(b);

    if (b == t) {
        if (!top_.compare_exchange_strong(t, t + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            bottom_.store(b + 1, std::memory_order_release);
            return false;
        }
        // CAS succeeded: we removed the last item (at b == t). Update bottom_ to keep
        // the deque consistent. If we don't, bottom_ < top_ causes push to overwrite
        // the stolen slot when it sees b < t and thinks the deque is empty.
        bottom_.store(b + 1, std::memory_order_release);
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
        return false;
    }

    return true;
}

template<typename T>
size_t ChaselevDeque<T>::size_approx() const {
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_relaxed);
    return static_cast<size_t>(b >= t ? b - t : 0);
}

class MultiPriorityWorkQueue {
public:
    explicit MultiPriorityWorkQueue(uint32_t priority_levels = 4) : levels_(priority_levels) {}

    void push(uint8_t priority, WorkItem item) {
        levels_[priority].push_bottom(item);
    }

    bool pop(WorkItem& out) {
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

}
