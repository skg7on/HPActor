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

// tests/sched/test_chaselev_deque.cpp
#include <cassert>
#include <hpactor/sched/work_queue.hpp>
#include <random>
#include <thread>
#include <vector>

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
    hpactor::sched::ChaselevDeque<Item> deque2(4); // small initial capacity
    for (int i = 0; i < 128; ++i) {
        deque2.push_bottom(Item{i});
    }
    assert(deque2.size_approx() == 128);
    // Chase-Lev pop_bottom is LIFO: newest item is popped first
    for (int i = 127; i >= 0; --i) {
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
        while (!start.load(std::memory_order_acquire)) { /* spin */
        }
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
    int owner_drained = 0;
    while (deque3.pop_bottom(out)) {
        ++owner_drained;
    }
    int total = steal_count.load() + owner_drained;
    assert(total == push_count.load());

    return 0;
}