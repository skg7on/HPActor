// tests/sched/test_chaselev_deque.cpp
#include <cassert>
#include <thread>
#include <vector>
#include <random>
#include <hpactor/sched/work_queue.hpp>

struct Item {
    int value;
};

int main() {
    // Test 1: basic push/pop
    hpactor::sched::ChaselevDeque<Item> deque;
    assert(deque.size_approx() == 0);

    deque.push_bottom(Item{42});
    assert(deque.size_approx() == 1);

    Item out{0};
    bool popped = deque.pop_bottom(out);
    assert(popped && out.value == 42);
    assert(deque.size_approx() == 0);

    // Test 2: steal returns false on empty
    bool stolen = deque.steal_top(out);
    assert(!stolen);

    // Test 3: fill beyond initial capacity and drain
    hpactor::sched::ChaselevDeque<Item> deque2(4);  // small initial capacity
    for (int i = 0; i < 128; ++i) {
        deque2.push_bottom(Item{i});
    }
    assert(deque2.size_approx() == 128);
    for (int i = 0; i < 128; ++i) {
        deque2.pop_bottom(out);
        assert(out.value == i);
    }
    assert(deque2.size_approx() == 0);

    // Test 4: concurrent push_bottom (owner) and steal_top (thief)
    hpactor::sched::ChaselevDeque<Item> deque3;
    std::atomic<bool> start{false};
    std::atomic<int> steal_count{0};
    std::atomic<int> push_count{0};

    std::thread thief([&]() {
        while (!start.load(std::memory_order_acquire)) { /* spin */ }
        for (int i = 0; i < 1000; ++i) {
            if (deque3.steal_top(out)) {
                steal_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (int i = 0; i < 1000; ++i) {
        deque3.push_bottom(Item{i});
        push_count.fetch_add(1, std::memory_order_relaxed);
    }
    thief.join();

    // All 1000 items either popped by owner or stolen
    int remaining = 0;
    while (deque3.pop_bottom(out)) { ++remaining; }
    int total = steal_count.load() + (1000 - remaining);
    assert(total == 1000);

    return 0;
}