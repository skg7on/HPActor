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

// Relacy must be the first include — it provides rl::atomic and also
// injects using-declarations into namespace std (std::atomic, etc.)
#include <relacy/relacy_std.hpp>

// int64_t is pulled in transitively by the real <atomic> but Relacy's
// fake <atomic> doesn't. Include it explicitly for MPSCMailbox's count_.
#include <cstdint>

// #include <atomic> in mpsc_mailbox.hpp resolves to Relacy's
// fakestd/atomic header (via -I in CMake), which re-declares
// std::atomic as using rl::atomic. No conflict with the system header.
#include <hpactor/mailbox/mpsc_mailbox.hpp>

// ==========================================================================
// Test node for MPSCMailbox<T>. Must provide std::atomic<T*> mpsc_next.
// Under Relacy, std::atomic is rl::atomic — all ops are instrumented.
// ==========================================================================
struct TestNode {
    std::atomic<TestNode *> mpsc_next{nullptr};
    int value = 0;

    TestNode() = default;
    explicit TestNode(int v) : value(v) {}
};

using Queue = hpactor::mailbox::MPSCMailbox<TestNode>;

#define RELACY_YIELD() rl::yield(1, $)

// ==========================================================================
// Suite 1 — 2 producers (2 items each), 1 consumer (bounded retries).
// Consumer deletes nodes. Verifies the fix for the CAS-failure
// use-after-free race.
// ==========================================================================
struct MPSC_2Producers : rl::test_suite<MPSC_2Producers, 3> {
    Queue queue;
    std::atomic<int> enqueued{0};
    std::atomic<int> dequeued{0};

    void thread(unsigned index) {
        if (index == 0) {
            for (int retry = 0; retry < 500; retry++) {
                TestNode *n = queue.dequeue();
                if (n) {
                    dequeued.fetch_add(1, std::memory_order_relaxed);
                    delete n;
                } else {
                    RELACY_YIELD();
                }
            }
        } else {
            for (int i = 0; i < 2; i++) {
                queue.enqueue(new TestNode(static_cast<int>(index)));
                enqueued.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void after() {
        int e = enqueued.load(std::memory_order_acquire);
        int d = dequeued.load(std::memory_order_acquire);
        RL_ASSERT(e >= d);
        RL_ASSERT(queue.count() == e - d);
        RL_ASSERT(queue.count() >= 0);
        TestNode *n;
        while ((n = queue.dequeue())) delete n;
        RL_ASSERT(queue.empty());
    }
};

// ==========================================================================
// Suite 2 — 4 producers (1 item each), 1 consumer.
// ==========================================================================
struct MPSC_4Producers : rl::test_suite<MPSC_4Producers, 5> {
    Queue queue;
    std::atomic<int> enqueued{0};
    std::atomic<int> dequeued{0};

    void thread(unsigned index) {
        if (index == 0) {
            for (int retry = 0; retry < 400; retry++) {
                TestNode *n = queue.dequeue();
                if (n) {
                    dequeued.fetch_add(1, std::memory_order_relaxed);
                    delete n;
                } else {
                    RELACY_YIELD();
                }
            }
        } else {
            queue.enqueue(new TestNode(static_cast<int>(index)));
            enqueued.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void after() {
        int e = enqueued.load(std::memory_order_acquire);
        int d = dequeued.load(std::memory_order_acquire);
        RL_ASSERT(e >= d);
        RL_ASSERT(queue.count() == e - d);
        TestNode *n;
        while ((n = queue.dequeue())) delete n;
        RL_ASSERT(queue.empty());
    }
};

// ==========================================================================
// Suite 3 — 2 producers (1 item each), 1 consumer that fetches exactly 1.
// Minimal test for the concurrent enqueue + partial dequeue case.
// ==========================================================================
struct MPSC_FetchOne : rl::test_suite<MPSC_FetchOne, 3> {
    Queue queue;

    void thread(unsigned index) {
        if (index == 0) {
            TestNode *n = nullptr;
            for (int retry = 0; retry < 400 && n == nullptr; retry++) {
                n = queue.dequeue();
                if (n == nullptr) RELACY_YIELD();
            }
            if (n) delete n;
        } else {
            queue.enqueue(new TestNode(static_cast<int>(index)));
        }
    }

    void after() {
        TestNode *n;
        while ((n = queue.dequeue())) delete n;
        RL_ASSERT(queue.empty());
    }
};

// ==========================================================================
// Suite 4 — Enqueue-only then drain. Tests multi-producer correctness
// without concurrent dequeue, then verifies the full chain is intact.
// ==========================================================================
struct MPSC_EnqueueDrain : rl::test_suite<MPSC_EnqueueDrain, 3> {
    Queue queue;

    void thread(unsigned /*index*/) {
        for (int i = 0; i < 2; i++)
            queue.enqueue(new TestNode(0));
    }

    void after() {
        RL_ASSERT(queue.count() == 6);
        for (int i = 0; i < 6; i++) {
            TestNode *n = queue.dequeue();
            RL_ASSERT(n != nullptr);
            delete n;
        }
        RL_ASSERT(queue.empty());
    }
};

int main() {
    rl::test_params params;
    params.search_type = rl::sched_random;
    params.iteration_count = 100000;
    params.execution_depth_limit = 50000;

    rl::simulate<MPSC_2Producers>(params);
    rl::simulate<MPSC_4Producers>(params);
    rl::simulate<MPSC_FetchOne>(params);
    rl::simulate<MPSC_EnqueueDrain>(params);

    return 0;
}
