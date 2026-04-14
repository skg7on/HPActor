// src/sched/work_queue.cpp
#include <hpactor/sched/work_queue.hpp>

namespace hpactor::sched {

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
    int64_t b = bottom_.load(std::memory_order_acquire) - 1;
    bottom_.store(b, std::memory_order_release);
    int64_t t = top_.load(std::memory_order_acquire);

    auto* arr = array_.load(std::memory_order_acquire);
    if (b - t < 0) {
        bottom_.store(t, std::memory_order_release);
        return false;
    }

    out = arr->get(b);

    int64_t expected_t = t;
    if (b == t) {
        if (!top_.compare_exchange_strong(expected_t, t + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            // Lost CAS — value lost to race
        }
        bottom_.store(t + 1, std::memory_order_release);
        return false;
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

// --- MultiPriorityWorkQueue ---
MultiPriorityWorkQueue::MultiPriorityWorkQueue(uint32_t priority_levels)
    : levels_(priority_levels) {}

void MultiPriorityWorkQueue::push(uint8_t priority, WorkItem item) {
    levels_[priority].push_bottom(item);
}

bool MultiPriorityWorkQueue::pop(WorkItem& out) {
    // Scan from highest priority to lowest
    for (uint32_t i = 0; i < levels_.size(); ++i) {
        if (levels_[i].pop_bottom(out)) {
            return true;
        }
    }
    return false;
}

size_t MultiPriorityWorkQueue::depth_approx() const {
    size_t total = 0;
    for (const auto& level : levels_) {
        total += level.size_approx();
    }
    return total;
}

} // namespace hpactor::sched
