# Actor Scheduling Subsystem Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the simple mutex-based `Scheduler` with the 5-phase hybrid scheduler (Chase-Lev deques, WorkerThread pool, EDF, A2WS, timing wheels, coroutine frame pool).

**Architecture:** Incremental replacement. Each phase leaves the system compilable and working. Phase 1 adds lock-free deques under a new `sched` namespace. Phase 2 swaps in the new scheduler behind an `IScheduler` interface. Phases 3-5 add EDF, A2WS, timers, and coroutine pools as discrete orthogonal additions.

**Tech Stack:** C++20, no exceptions, no RTTI, lock-free data structures, CMake/Ninja

---

## File Structure

```
include/hpactor/
├── sched/
│   ├── work_queue.hpp       # [NEW] ChaselevDeque, WorkItem, MultiPriorityWorkQueue
│   └── scheduler.hpp        # [NEW] IScheduler, HybridScheduler, TimerHandle
└── timer/
    ├── timing_wheel.hpp     # [NEW Phase 4] HierarchicalTimingWheel, TimerEntry
    └── timer_entry.hpp      # [NEW Phase 4] TimerEntry (intrusive node)
include/hpactor/memory/
    └── coroutine_frame_pool.hpp  # [NEW Phase 5]

src/
├── sched/
│   ├── work_queue.cpp       # [NEW] ChaselevDeque growth logic
│   ├── scheduler.cpp        # [NEW] HybridScheduler methods
│   ├── worker_thread.cpp    # [NEW Phase 2] WorkerThread run_loop
│   ├── edf_queue.cpp        # [NEW Phase 3]
│   └── a2ws.cpp             # [NEW Phase 3]
src/timer/
    └── timing_wheel.cpp     # [NEW Phase 4]
src/memory/
    └── coroutine_frame_pool.cpp  # [NEW Phase 5]

tests/
└── sched/                   # [NEW] tests/sched/ subdirectory
    ├── test_chaselev_deque.cpp
    ├── test_multi_priority_work_queue.cpp
    ├── test_hybrid_scheduler.cpp  # Phase 2+
    └── test_edf_a2ws.cpp          # Phase 3
```

**Modified:**
- `CMakeLists.txt` — add new source files per phase
- `include/hpactor/core/actor_system.hpp` — swap `Scheduler*` for `IScheduler*`
- `src/actor/actor_system.cpp` — update scheduler lifecycle

---

## Phase 1: Chase-Lev Deques + Multi-Priority Work Queue

### Task 1.1: Create `tests/sched/` directory and Phase 1 test for `ChaselevDeque`

**Files:**
- Create: `tests/sched/test_chaselev_deque.cpp`
- Test: `tests/mailbox/test_mailbox_stress.cpp` (for reference on stress test patterns)

**Tasks:**
- [ ] **Step 1: Create `tests/sched/` directory**

```bash
mkdir -p tests/sched
```

- [ ] **Step 2: Write failing test for `ChaselevDeque`**

```cpp
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
```

- [ ] **Step 3: Verify it compiles (expected: fail — header doesn't exist yet)**

```bash
g++ -std=c++20 -I include tests/sched/test_chaselev_deque.cpp -o /dev/null 2>&1 | head -5
# Expected: error: hpactor/sched/work_queue.hpp: No such file or directory
```

---

### Task 1.2: Create `include/hpactor/sched/work_queue.hpp`

**Files:**
- Create: `include/hpactor/sched/work_queue.hpp`
- Depends on: Task 1.1

**Tasks:**
- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/sched/work_queue.hpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace hpactor::sched {

// WorkItem: unit of scheduling work enqueued to a worker deque
struct WorkItem {
    ActorId  actor;
    int64_t  deadline_ns;   // absolute CLOCK_MONOTONIC; INT64_MAX if none
    uint64_t sequence;      // FIFO tiebreaker within same deadline
};

// Forward declaration (ActorId lives in types.hpp)
struct ActorId;

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

    int64_t circular_index(int64_t i) const { return i & array_.load(std::memory_order_acquire)->mask; }

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

    // Push to a specific priority level. Owner thread only.
    void push(uint8_t priority, WorkItem item);

    // Pop from the highest non-empty level. Owner thread only. Returns false if empty.
    bool pop(WorkItem& out);

    // Approximate total depth across all levels (relaxed read).
    size_t depth_approx() const;

    uint32_t num_levels() const { return static_cast<uint32_t>(levels_.size()); }

private:
    std::vector<ChaselevDeque<WorkItem>> levels_;
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Verify it compiles**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/sched/work_queue.hpp -o /dev/null 2>&1
# Expected: no errors
```

---

### Task 1.3: Create `src/sched/work_queue.cpp`

**Files:**
- Create: `src/sched/work_queue.cpp`
- Depends on: Task 1.2

**Tasks:**
- [ ] **Step 1: Write the implementation**

```cpp
// src/sched/work_queue.cpp
#include <hpactor/sched/work_queue.hpp>

namespace hpactor::sched {

// --- CircularArray ---
template<typename T>
CircularArray<T>::CircularArray(size_t cap) : buf(cap), mask(cap - 1) {
    // All slots start with default-constructed T (acceptable for WorkItem)
    for (auto& slot : buf) {
        slot.store(T{}, std::memory_order_relaxed);
    }
}

template<typename T>
CircularArray<T>* CircularArray<T>::grow(int64_t bottom, int64_t top) const {
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
        // Overflow — grow
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
        // Empty after this pop
        bottom_.store(t, std::memory_order_release);
        return false;
    }

    out = arr->get(b);

    int64_t expected_t = t;
    if (b == t) {
        // Try to atomically advance top_ to mark as truly empty
        if (!top_.compare_exchange_strong(expected_t, t + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            // Lost CAS — another thief got it last moment; value is lost
            // (standard Chase-Lev semantics: item lost to CAS race)
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
        return false;  // empty
    }

    auto* arr = array_.load(std::memory_order_acquire);
    out = arr->get(t);

    if (!top_.compare_exchange_strong(t, t + 1,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
        return false;  // ABA race — lost CAS
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
template<typename T>
MultiPriorityWorkQueue<T>::MultiPriorityWorkQueue(uint32_t priority_levels)
    : levels_(priority_levels) {}

} // namespace hpactor::sched
```

> **Correction:** `MultiPriorityWorkQueue` is not a template — it operates only on `WorkItem`. The previous header declaration had it as a template. Fix the header before writing this .cpp.

- [ ] **Step 2: Fix header — `MultiPriorityWorkQueue` is not a template**

```cpp
// Replace the last section of work_queue.hpp MultiPriorityWorkQueue declaration with:
// (Non-template, only operates on WorkItem)

class MultiPriorityWorkQueue {
public:
    explicit MultiPriorityWorkQueue(uint32_t priority_levels = 4);

    void push(uint8_t priority, WorkItem item);   // owner only
    bool pop(WorkItem& out);                      // scans high→low, owner only
    size_t depth_approx() const;                  // sum of all level sizes

    uint32_t num_levels() const { return static_cast<uint32_t>(levels_.size()); }

private:
    std::vector<ChaselevDeque<WorkItem>> levels_;
};
```

- [ ] **Step 3: Verify the test compiles and runs**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include tests/sched/test_chaselev_deque.cpp src/sched/work_queue.cpp -o build/test_chaselev_deque 2>&1
# Expected: no errors
./build/test_chaselev_deque; echo "Exit code: $?"
# Expected: 0
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/sched/work_queue.hpp src/sched/work_queue.cpp tests/sched/test_chaselev_deque.cpp
git commit -m "feat(sched): add Chase-Lev lock-free deque and WorkItem

Implements ChaselevDeque<T> with O(1) owner push_bottom/pop_bottom and
lock-free steal_top (ABA-safe via version tag). MultiPriorityWorkQueue
provides P-level priority array for Phase 2 worker threads.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 1.4: Create Phase 1 test for `MultiPriorityWorkQueue`

**Files:**
- Create: `tests/sched/test_multi_priority_work_queue.cpp`
- Depends on: Task 1.3

**Tasks:**
- [ ] **Step 1: Write failing test**

```cpp
// tests/sched/test_multi_priority_work_queue.cpp
#include <cassert>
#include <hpactor/sched/work_queue.hpp>
#include <vector>

int main() {
    hpactor::sched::MultiPriorityWorkQueue q(4);

    // Test: pop returns false on empty
    hpactor::sched::WorkItem out;
    assert(!q.pop(out));

    // Test: push/pop round-trip
    hpactor::sched::WorkItem item;
    item.actor = hpactor::ActorId{1};
    item.deadline_ns = 1000;
    item.sequence = 1;

    q.push(0, item);
    assert(q.depth_approx() == 1);
    assert(q.pop(out));
    assert(out.actor.value() == 1);
    assert(q.depth_approx() == 0);

    // Test: priority ordering — higher priority returned first
    hpactor::sched::WorkItem lo, hi;
    lo.actor = hpactor::ActorId{1}; lo.deadline_ns = 2000; lo.sequence = 1;
    hi.actor = hpactor::ActorId{2}; hi.deadline_ns = 1000; hi.sequence = 2;

    q.push(3, lo);   // low priority (3)
    q.push(0, hi);   // high priority (0)
    q.push(2, lo);   // also low

    assert(q.pop(out));
    assert(out.actor.value() == 2);  // high priority first
    assert(q.pop(out));
    assert(out.actor.value() == 1);  // priority 2 before priority 3
    assert(q.pop(out));
    assert(out.actor.value() == 1);  // last remaining

    return 0;
}
```

- [ ] **Step 2: Build and run**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include tests/sched/test_multi_priority_work_queue.cpp src/sched/work_queue.cpp -o build/test_multi_priority_work_queue 2>&1
./build/test_multi_priority_work_queue; echo "Exit: $?"
# Expected: 0
```

- [ ] **Step 3: Commit**

```bash
git add tests/sched/test_multi_priority_work_queue.cpp
git commit -m "test(sched): add MultiPriorityWorkQueue priority ordering test

Verifies push/pop semantics and that highest-priority non-empty level
is always selected first.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 1.5: Update `CMakeLists.txt` for Phase 1

**Files:**
- Modify: `CMakeLists.txt:59-84` (hpactor_lib sources)
- Depends on: Tasks 1.2, 1.3

**Tasks:**
- [ ] **Step 1: Add Phase 1 source files to `hpactor_lib`**

In `CMakeLists.txt`, after `src/actor/scheduler.cpp` line, add:

```cmake
# Scheduling subsystem (Phase 1+)
src/sched/work_queue.cpp
```

Also add scheduling tests to `tests/CMakeLists.txt` (after the existing tests section):

```cmake
# =============================================================================
# Scheduling tests
# =============================================================================
add_executable(test_chaselev_deque sched/test_chaselev_deque.cpp)
target_link_libraries(test_chaselev_deque hpactor)
add_test(NAME test_chaselev_deque COMMAND test_chaselev_deque)

add_executable(test_multi_priority_work_queue sched/test_multi_priority_work_queue.cpp)
target_link_libraries(test_multi_priority_work_queue hpactor)
add_test(NAME test_multi_priority_work_queue COMMAND test_multi_priority_work_queue)
```

- [ ] **Step 2: Verify full build**

```bash
cmake -S . -B build -GNinja 2>&1 | tail -10
ninja -C build test_chaselev_deque test_multi_priority_work_queue 2>&1
ctest -R "chaselev|multi_priority" --output-on-failure
# Expected: both pass
```

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt
git commit -m "build: add sched/ sources and tests to CMake

Phase 1: Chase-Lev deque and MultiPriorityWorkQueue now in build.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 2: Worker Thread + IScheduler Interface

### Task 2.1: Create `include/hpactor/sched/scheduler.hpp`

**Files:**
- Create: `include/hpactor/sched/scheduler.hpp`
- Depends on: Task 1.5

**Tasks:**
- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/sched/scheduler.hpp
#pragma once

#include <hpactor/sched/work_queue.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace hpactor {

// Forward declaration (ActorId already in types.hpp, include via work_queue.hpp)
class ActorSystem;

namespace sched {

// Timer handle — opaque token returned by schedule_after/schedule_every
struct TimerHandle {
    uint64_t id = 0;
    bool valid() const noexcept { return id != 0; }
};

using timer_callback = std::function<void()>;

// IScheduler: abstract interface for the actor scheduler.
// All methods are thread-safe unless noted otherwise.
// The interface uses only ActorId (a ::types concept) — no actor/net types.
class IScheduler {
public:
    virtual ~IScheduler() = default;

    // Start all worker threads. Must be called before any notify_ready().
    virtual void start() = 0;

    // Stop and join all worker threads.
    virtual void stop() = 0;

    // Notify that `actor` has messages waiting in its mailbox.
    // May be called from any thread including I/O threads (thread-safe).
    // priority: 0 = highest, higher = lower priority
    // deadline_ns: absolute CLOCK_MONOTONIC deadline, INT64_MAX = none
    virtual void notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;

    // Hint that `actor` has exhausted its mailbox and is idle.
    virtual void notify_idle(ActorId actor) = 0;

    // Schedule a one-shot callback after `delay_ns` nanoseconds.
    virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;

    // Schedule a repeating callback every `interval_ns` nanoseconds.
    virtual TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) = 0;

    // Cancel a pending timer. Safe to call after the timer has already fired.
    virtual void cancel_timer(TimerHandle handle) = 0;

    // Number of worker threads.
    virtual size_t worker_count() const = 0;
};

// HybridScheduler: concrete scheduler with per-worker priority deques.
// For Phase 2, routing is round-robin per priority (no EDF/A2WS yet).
// EDF + A2WS are added in Phase 3.
class HybridScheduler final : public IScheduler {
public:
    struct Config {
        size_t  num_workers          = std::thread::hardware_concurrency();
        uint32_t priority_levels     = 4;
        uint32_t steal_threshold     = 2;   // τ: Phase 3
        uint32_t timer_resolution_us = 1000; // Phase 4 stub
    };

    explicit HybridScheduler(class ActorSystem& system, Config cfg = {});
    ~HybridScheduler() override;

    // IScheduler
    void        start()                                              override;
    void        stop()                                               override;
    void        notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) override;
    void        notify_idle(ActorId actor)                            override;
    TimerHandle schedule_after(timer_callback cb, int64_t delay_ns)  override;
    TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) override;
    void        cancel_timer(TimerHandle handle)                      override;
    size_t      worker_count() const                                  override;

    // Diagnostic: approximate total ready-queue depth.
    size_t approx_queue_depth() const;

private:
    // Select worker index for a given priority using round-robin.
    // Phase 3 upgrades this to read A2WS snapshots.
    size_t select_worker(uint8_t priority) const;

    class ActorSystem&                  system_;
    Config                              cfg_;
    std::vector<class WorkerThread>      workers_;
    std::atomic<bool>                   running_{false};
    std::atomic<uint64_t>              next_sequence_{0};

    // Per-priority round-robin counters (atomic for potential Phase 3 use)
    std::vector<std::atomic<size_t>>    rr_counters_;
};

} // namespace sched
} // namespace hpactor
```

- [ ] **Step 2: Verify it compiles**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/sched/scheduler.hpp -o /dev/null 2>&1
# Expected: error: 'ActorSystem' has not been declared yet (forward decl needed)
```

- [ ] **Step 3: Fix forward declaration**

The `HybridScheduler` constructor takes `class ActorSystem&`. In the class body, `class ActorSystem&` needs to be declared before use. Move the `class ActorSystem& system_;` member declaration after the constructor declaration, or add a forward declaration at the top of the namespace block:

```cpp
namespace hpactor {
class ActorSystem;  // forward

namespace sched {
// ... rest unchanged
```

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/sched/scheduler.hpp -o /dev/null 2>&1
# Expected: no errors
```

---

### Task 2.2: Create `include/hpactor/sched/worker_thread.hpp`

**Files:**
- Create: `include/hpactor/sched/worker_thread.hpp`
- Depends on: Task 2.1

**Tasks:**
- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/sched/worker_thread.hpp
#pragma once

#include <hpactor/sched/work_queue.hpp>

#include <atomic>
#include <thread>

namespace hpactor::sched {

// WorkerThread: one OS thread with a local multi-priority deque.
// Owned by HybridScheduler; not copyable or movable (owns a std::thread).
class WorkerThread {
public:
    WorkerThread(size_t index, class HybridScheduler& owner, const HybridScheduler::Config& cfg);
    ~WorkerThread();

    WorkerThread(const WorkerThread&)            = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
    WorkerThread(WorkerThread&&)                 = delete;
    WorkerThread& operator=(WorkerThread&&)      = delete;

    void start();
    void stop();                      // sets stop flag, joins thread

    // Called by HybridScheduler::notify_ready routing (owner side push)
    void push(WorkItem item);

    // Called by a thief to steal work from this worker
    bool try_steal(WorkItem& out);

    // Called by A2WS to sample approximate load
    size_t load_snapshot() const { return load_.load(std::memory_order_relaxed); }

    size_t index() const { return index_; }

private:
    void run_loop();
    bool try_execute_one();   // pop from local queues and dispatch; returns false if empty
    // Phase 3: void try_steal_work(); — steals from A2WS victims

    size_t                         index_;
    class HybridScheduler&          owner_;
    MultiPriorityWorkQueue          queues_;
    std::thread                    thread_;
    std::atomic<bool>              stop_{false};

    // Approximate queue depth; updated after each pop/push/steal.
    // alignas(64) prevents false sharing with neighboring cache lines.
    alignas(64) std::atomic<uint32_t> load_{0};
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Verify it compiles**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/sched/worker_thread.hpp -o /dev/null 2>&1
# Expected: no errors
```

---

### Task 2.3: Create `src/sched/worker_thread.cpp`

**Files:**
- Create: `src/sched/worker_thread.cpp`
- Depends on: Task 2.2

**Tasks:**
- [ ] **Step 1: Write WorkerThread implementation**

```cpp
// src/sched/worker_thread.cpp
#include <hpactor/sched/worker_thread.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/core/actor_system.hpp>
#include <iostream>
#include <chrono>

namespace hpactor::sched {

WorkerThread::WorkerThread(size_t index, class HybridScheduler& owner,
                           const HybridScheduler::Config& cfg)
    : index_(index), owner_(owner), queues_(cfg.priority_levels) {}

WorkerThread::~WorkerThread() {
    stop();
}

void WorkerThread::start() {
    thread_ = std::thread([this] { run_loop(); });
}

void WorkerThread::stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void WorkerThread::push(WorkItem item) {
    queues_.push(static_cast<uint8_t>(0), item);  // use priority 0 for now (Phase 2)
    load_.store(static_cast<uint32_t>(queues_.depth_approx()), std::memory_order_relaxed);
}

bool WorkerThread::try_steal(WorkItem& out) {
    // Phase 3: steal from the highest non-empty priority level
    // For Phase 2, just return false (no stealing yet)
    (void)out;
    return false;
}

bool WorkerThread::try_execute_one() {
    WorkItem item;
    if (!queues_.pop(item)) {
        return false;
    }

    load_.store(static_cast<uint32_t>(queues_.depth_approx()), std::memory_order_relaxed);

    // Dispatch to actor
    auto actor = owner_.system().get_actor(item.actor);
    if (!actor) {
        return true;  // actor gone, discard
    }

    auto* mailbox = owner_.system().get_mailbox(item.actor);
    if (!mailbox) {
        return true;
    }

    // Drain mailbox and process all messages
    // (Phase 2: simple loop; Phases 3-5 handle EDF ordering, I/O waiting, coroutines)
    hpactor::Message<hpactor::MessageVariant> msg;
    while (mailbox->try_pop(msg)) {
        actor->receive(msg.move_payload());
    }

    // If mailbox still has messages, re-enqueue
    if (!mailbox->empty()) {
        queues_.push(0, WorkItem{item.actor, INT64_MAX, 0});
    }

    return true;
}

void WorkerThread::run_loop() {
    while (!stop_.load(std::memory_order_acquire)) {
        if (!try_execute_one()) {
            // Phase 2: no work and couldn't steal — yield
            std::this_thread::yield();
        }
    }
}

} // namespace hpactor::sched
```

> **Note:** `owner_.system()` — `HybridScheduler` has `ActorSystem& system_` as a private member. Expose it via a `system()` accessor, or make `WorkerThread` a friend. Add to `HybridScheduler`:
> ```cpp
> friend class WorkerThread;
> class ActorSystem& system() { return system_; }
> ```

- [ ] **Step 2: Verify it compiles**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c src/sched/worker_thread.cpp -o /dev/null 2>&1
# Expected: errors — fix as noted above (friend declaration + accessor)
```

- [ ] **Step 3: Fix `scheduler.hpp` — add friend + accessor**

```cpp
// In HybridScheduler class body, add:
friend class WorkerThread;
class ActorSystem& system() { return system_; }
```

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c src/sched/worker_thread.cpp -o /dev/null 2>&1
# Expected: no errors (if MessageVariant and ActorSystem are complete types here)
# If not, add stub implementations or forward declare more types
```

> **Stub note:** `ActorSystem::get_actor()` and `ActorSystem::get_mailbox()` are declared in `actor_system.hpp`. Since `src/actor/actor_system.cpp` is already in the build, those symbols exist. `MessageVariant` is likely a `std::variant` defined in `message.hpp`.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/sched/worker_thread.hpp src/sched/worker_thread.cpp
git commit -m "feat(sched): add WorkerThread with run_loop and local work queue

WorkerThread owns a MultiPriorityWorkQueue and processes WorkItems by
draining the actor's mailbox. Phase 2: round-robin routing and simple
yield on idle. Phase 3 will add A2WS stealing.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 2.4: Create `src/sched/scheduler.cpp`

**Files:**
- Create: `src/sched/scheduler.cpp`
- Depends on: Tasks 2.1, 2.3

**Tasks:**
- [ ] **Step 1: Write `HybridScheduler` implementation**

```cpp
// src/sched/scheduler.cpp
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/sched/worker_thread.hpp>

namespace hpactor::sched {

HybridScheduler::HybridScheduler(class ActorSystem& system, Config cfg)
    : system_(system), cfg_(cfg), workers_(cfg.num_workers), rr_counters_(cfg.priority_levels) {
    for (size_t i = 0; i < cfg_.num_workers; ++i) {
        workers_[i] = WorkerThread(i, *this, cfg_);
    }
}

HybridScheduler::~HybridScheduler() {
    stop();
}

void HybridScheduler::start() {
    running_.store(true, std::memory_order_release);
    for (size_t i = 0; i < workers_.size(); ++i) {
        workers_[i].start();
    }
}

void HybridScheduler::stop() {
    running_.store(false, std::memory_order_release);
    for (auto& w : workers_) {
        w.stop();
    }
}

size_t HybridScheduler::select_worker(uint8_t priority) const {
    // Round-robin per priority level
    size_t idx = priority % workers_.size();
    size_t old = rr_counters_[idx].fetch_add(1, std::memory_order_relaxed);
    return old % workers_.size();
}

void HybridScheduler::notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    WorkItem item{actor, deadline_ns, next_sequence_.fetch_add(1, std::memory_order_relaxed)};
    size_t worker_idx = select_worker(priority);
    workers_[worker_idx].push(item);
}

void HybridScheduler::notify_idle(ActorId /*actor*/) {
    // Phase 3: remove from EDF queue. For Phase 2, no global state to clean up.
}

TimerHandle HybridScheduler::schedule_after(timer_callback /*cb*/, int64_t /*delay_ns*/) {
    // Phase 4 stub — return invalid handle
    return TimerHandle{0};
}

TimerHandle HybridScheduler::schedule_every(timer_callback /*cb*/, int64_t /*interval_ns*/) {
    // Phase 4 stub
    return TimerHandle{0};
}

void HybridScheduler::cancel_timer(TimerHandle /*handle*/) {
    // Phase 4 stub
}

size_t HybridScheduler::worker_count() const {
    return workers_.size();
}

size_t HybridScheduler::approx_queue_depth() const {
    size_t total = 0;
    for (const auto& w : workers_) {
        total += w.load_snapshot();
    }
    return total;
}

} // namespace hpactor::sched
```

- [ ] **Step 2: Verify it compiles**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c src/sched/scheduler.cpp -o /dev/null 2>&1
# Expected: no errors (ActorSystem forward declaration resolves through the friend/accessor)
```

---

### Task 2.5: Wire `IScheduler` into `ActorSystem`

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`, `src/actor/actor_system.cpp`
- Depends on: Tasks 2.1, 2.4

**Tasks:**
- [ ] **Step 1: Replace `Scheduler` with `IScheduler` in `ActorSystem`**

In `include/hpactor/core/actor_system.hpp`:
- Change `#include "core/scheduler.hpp"` to `#include "sched/scheduler.hpp"`
- Change `class Scheduler;` forward declaration to `class IScheduler;`
- Change `std::unique_ptr<Scheduler> scheduler_` to `std::unique_ptr<sched::IScheduler> scheduler_`

- [ ] **Step 2: Update `ActorSystem::deliver_local()` to call `scheduler_->notify_ready()` with priority/deadline**

The current `ActorSystem::deliver_local()` signature is:
```cpp
void deliver_local(ActorId target, MessageVariant msg);
```
It calls `scheduler_->enqueue(target, msg)` with only 2 args. `IScheduler::notify_ready()` requires 3 args `(ActorId, uint8_t priority, int64_t deadline_ns)`.

**For Phase 2**, use defaults: `priority = 0` (highest), `deadline_ns = INT64_MAX` (no real-time deadline). Update `deliver_local()`:

```cpp
// In src/actor/actor_system.cpp
void ActorSystem::deliver_local(ActorId target, MessageVariant /*msg*/) {
    // ... existing mailbox delivery ...
    scheduler_->notify_ready(target, 0, INT64_MAX);
}
```

> **Note:** Phase 3 will thread per-actor priority/deadline from `ActorContext::send()` through to `notify_ready()`. Phase 2 uses the simplest possible wiring with hardcoded defaults.

> **Architectural note:** `ActorContext::send()` is called from actor code (inside `Behavior::invoke`). The priority/deadline needs to come from the actor's `ActorConfig` or be passed as a parameter. For Phase 2, the caller of `deliver_local()` provides these values — since actors are currently spawned without explicit scheduling parameters, all use the Phase 2 defaults above.

- [ ] **Step 3: Replace `Scheduler` instantiation with `HybridScheduler` in `ActorSystem` constructor**

In `src/actor/actor_system.cpp`:

```cpp
// Replace:
// scheduler_ = std::make_unique<Scheduler>(*this, config_.scheduler_threads);
// With:
scheduler_ = std::make_unique<sched::HybridScheduler>(*this, sched::HybridScheduler::Config{
    .num_workers = config_.scheduler_threads
});
scheduler_->start();
```

Also update the destructor to call `scheduler_->stop()` if needed (already handled in `~Scheduler`).

- [ ] **Step 4: Verify build**

```bash
ninja -C build 2>&1 | tail -20
# Expected: link errors if ActorSystem methods reference old Scheduler type
# Fix any remaining references
```

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat(sched): wire HybridScheduler into ActorSystem via IScheduler

ActorSystem now holds IScheduler*. HybridScheduler is instantiated with
config_.scheduler_threads workers. Existing Scheduler class left intact
for reference until Phase 2 verification is complete.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 2.6: Create Phase 2 integration test

**Files:**
- Create: `tests/sched/test_hybrid_scheduler.cpp`
- Depends on: Task 2.5

> **Note:** The existing `Behavior` class uses `std::function<void(MessageVariant&&)>` — no fluent `.on<T>()` builder. `ActorContext::reply()` is a stub. This test focuses on scheduler lifecycle and routing without those.

**Tasks:**
- [ ] **Step 1: Write scheduler lifecycle test**

```cpp
// tests/sched/test_hybrid_scheduler.cpp
#include <cassert>
#include <thread>
#include <chrono>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>

class TestActor : public hpactor::EventBasedActor {
public:
    TestActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

int main() {
    // Test 1: ActorSystem starts and stops cleanly with HybridScheduler
    {
        hpactor::Config config;
        config.scheduler_threads = 2;
        hpactor::ActorSystem system(config);
        assert(system.scheduler() != nullptr);
    }

    // Test 2: Spawn several actors — they get unique valid ActorIds
    {
        hpactor::Config config;
        config.scheduler_threads = 4;
        hpactor::ActorSystem system(config);

        auto a1 = system.spawn<TestActor>();
        auto a2 = system.spawn<TestActor>();
        auto a3 = system.spawn<TestActor>();

        assert(a1.id().value() != 0);
        assert(a2.id().value() != 0);
        assert(a3.id().value() != 0);
        assert(a1.id() != a2.id());
        assert(a2.id() != a3.id());
    }

    // Test 3: Scheduler worker count matches config
    {
        hpactor::Config config;
        config.scheduler_threads = 3;
        hpactor::ActorSystem system(config);
        assert(system.scheduler()->worker_count() == 3);
    }

    return 0;
}
```

- [ ] **Step 2: Add `scheduler()` accessor to `ActorSystem`**

In `include/hpactor/core/actor_system.hpp`, add:
```cpp
sched::IScheduler* scheduler() { return scheduler_.get(); }
```
Also add `#include "sched/scheduler.hpp"` at the top.

- [ ] **Step 3: Build and run**

```bash
ninja -C build test_hybrid_scheduler 2>&1 | tail -20
./build/test_hybrid_scheduler; echo "Exit: $?"
# Expected: 0
```

- [ ] **Step 4: Commit**

```bash
git add tests/sched/test_hybrid_scheduler.cpp
git commit -m "test(sched): add hybrid scheduler lifecycle integration test

Verifies HybridScheduler starts/stops cleanly, actors get unique ids,
and worker_count() matches config. Full actor messaging (Behavior::on<T>(),
reply()) deferred to future phase.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

## Phase 3: EDF Priority Queue + A2WS Load Balancing

### Task 3.1: Create `include/hpactor/sched/edf_queue.hpp`

**Files:**
- Create: `include/hpactor/sched/edf_queue.hpp`
- Depends on: Phase 2 complete

**Tasks:**
- [ ] **Step 1: Write header**

```cpp
// include/hpactor/sched/edf_queue.hpp
#pragma once

#include <atomic>
#include <vector>
#include <hpactor/sched/work_queue.hpp>  // ActorId

namespace hpactor::sched {

// Global EDF view: tracks all actors that have been notified as ready,
// ordered by (priority ASC, deadline_ns ASC).
// Used by HybridScheduler to route notify_ready() to the right worker.
// Thread-safe via brief spinlock on upsert/remove; peek/pop acquire lock.
class EDFPriorityQueue {
public:
    struct EDFEntry {
        ActorId  actor;
        uint8_t  priority;
        int64_t  deadline_ns;
        size_t   target_worker;  // assigned worker index
    };

    EDFPriorityQueue();
    ~EDFPriorityQueue();

    // Insert or update an actor's entry. Thread-safe (spinlocked).
    void upsert(EDFEntry entry);

    // Remove an actor (called when it becomes idle). Thread-safe.
    void remove(ActorId actor);

    // Peek at the minimum entry without popping. Returns false if empty.
    bool peek_min(EDFEntry& out);

    // Pop the minimum entry. Returns false if empty.
    bool pop_min(EDFEntry& out);

    size_t size() const;

private:
    void sift_up(size_t idx);
    void sift_down(size_t idx);

    static bool higher_priority(const EDFEntry& a, const EDFEntry& b);

    mutable std::atomic_flag        lock_ = ATOMIC_FLAG_INIT;
    std::vector<EDFEntry>           heap_;  // binary min-heap
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Verify it compiles**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/sched/edf_queue.hpp -o /dev/null 2>&1
# Expected: no errors
```

---

### Task 3.2: Create `src/sched/edf_queue.cpp`

**Files:**
- Create: `src/sched/edf_queue.cpp`
- Depends on: Task 3.1

**Tasks:**
- [ ] **Step 1: Write implementation**

```cpp
// src/sched/edf_queue.cpp
#include <hpactor/sched/edf_queue.hpp>
#include <algorithm>

namespace hpactor::sched {

EDFPriorityQueue::EDFPriorityQueue() : heap_() {}
EDFPriorityQueue::~EDFPriorityQueue() = default;

bool EDFPriorityQueue::higher_priority(const EDFEntry& a, const EDFEntry& b) {
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.deadline_ns < b.deadline_ns;
}

void EDFPriorityQueue::upsert(EDFEntry entry) {
    while (lock_.test_and_set(std::memory_order_acquire)) { /* spin */ }
    auto it = std::find_if(heap_.begin(), heap_.end(),
                           [&](const EDFEntry& e) { return e.actor == entry.actor; });
    if (it != heap_.end()) {
        *it = entry;
        // Could be out of heap order now — sift both directions
        sift_up(std::distance(heap_.begin(), it));
        sift_down(std::distance(heap_.begin(), it));
    } else {
        heap_.push_back(entry);
        sift_up(heap_.size() - 1);
    }
    lock_.clear(std::memory_order_release);
}

void EDFPriorityQueue::remove(ActorId actor) {
    while (lock_.test_and_set(std::memory_order_acquire)) { /* spin */ }
    auto it = std::find_if(heap_.begin(), heap_.end(),
                           [&](const EDFEntry& e) { return e.actor == actor; });
    if (it != heap_.end()) {
        *it = heap_.back();
        heap_.pop_back();
        if (it != heap_.end()) {
            sift_up(std::distance(heap_.begin(), it));
            sift_down(std::distance(heap_.begin(), it));
        }
    }
    lock_.clear(std::memory_order_release);
}

bool EDFPriorityQueue::peek_min(EDFEntry& out) {
    while (lock_.test_and_set(std::memory_order_acquire)) { /* spin */ }
    bool result = false;
    if (!heap_.empty()) {
        out = heap_[0];
        result = true;
    }
    lock_.clear(std::memory_order_release);
    return result;
}

bool EDFPriorityQueue::pop_min(EDFEntry& out) {
    while (lock_.test_and_set(std::memory_order_acquire)) { /* spin */ }
    bool result = false;
    if (!heap_.empty()) {
        out = heap_[0];
        result = true;
        heap_[0] = heap_.back();
        heap_.pop_back();
        if (!heap_.empty()) {
            sift_down(0);
        }
    }
    lock_.clear(std::memory_order_release);
    return result;
}

size_t EDFPriorityQueue::size() const {
    while (lock_.test_and_set(std::memory_order_acquire)) { /* spin */ }
    size_t result = heap_.size();
    lock_.clear(std::memory_order_release);
    return result;
}

void EDFPriorityQueue::sift_up(size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        if (!higher_priority(heap_[idx], heap_[parent])) break;
        std::swap(heap_[idx], heap_[parent]);
        idx = parent;
    }
}

void EDFPriorityQueue::sift_down(size_t idx) {
    size_t n = heap_.size();
    while (true) {
        size_t smallest = idx;
        size_t left = 2 * idx + 1;
        size_t right = 2 * idx + 2;
        if (left < n && higher_priority(heap_[left], heap_[smallest])) smallest = left;
        if (right < n && higher_priority(heap_[right], heap_[smallest])) smallest = right;
        if (smallest == idx) break;
        std::swap(heap_[idx], heap_[smallest]);
        idx = smallest;
    }
}

} // namespace hpactor::sched
```

- [ ] **Step 2: Verify it compiles and commit**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c src/sched/edf_queue.cpp -o /dev/null 2>&1
git add src/sched/edf_queue.cpp include/hpactor/sched/edf_queue.hpp
git commit -m "feat(sched): add EDFPriorityQueue with spinlocked binary min-heap

EDF ordering keyed by (priority ASC, deadline_ns ASC). Thread-safe upsert
and remove via brief spinlock. Used by HybridScheduler to track globally
ready actors.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 3.3: Create `include/hpactor/sched/a2ws.hpp`

**Files:**
- Create: `include/hpactor/sched/a2ws.hpp`
- Depends on: Task 3.2

**Tasks:**
- [ ] **Step 1: Write header**

```cpp
// include/hpactor/sched/a2ws.hpp
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace hpactor::sched {

// A2WSCoordinator: manages the ring-topology load snapshot array for Adaptive
// Asynchronous Work-Stealing. Each Snapshot is cache-line-aligned to prevent
// false sharing between workers.
class A2WSCoordinator {
public:
    explicit A2WSCoordinator(size_t num_workers, uint32_t steal_threshold = 2);

    // Update snapshot for a worker after it processes a work item.
    // `load` is the worker's current approximate queue depth.
    void update_snapshot(size_t worker_index, uint32_t load) noexcept;

    // Find the best steal victim for `requester`:
    //   1. Read left and right ring neighbors' snapshots.
    //   2. If imbalance > threshold, prefer the more-loaded neighbor.
    //   3. If neighbors are empty at the priority level, fall back to
    //      scanning all workers (not yet implemented — returns SIZE_MAX).
    //   Returns SIZE_MAX if no steal warranted.
    size_t best_victim(size_t requester, uint8_t /*priority*/) const noexcept;

    // Halving heuristic: how many items to steal from a victim.
    // Returns max(1, (victim_load - own_load) / 2).
    uint32_t steal_count(uint32_t own_load, uint32_t victim_load) const noexcept;

    // Returns true if system is in low-load mode (Σ load < α·m, α=2).
    bool is_low_load() const noexcept;

    // Number of workers permitted to steal concurrently in low-load mode.
    size_t max_concurrent_stealers() const noexcept;

private:
    size_t   num_workers_;
    uint32_t threshold_;

    struct alignas(64) Snapshot {
        std::atomic<uint32_t> load{0};
    };

    std::vector<Snapshot>        snapshots_;
    std::atomic<uint32_t>        active_stealers_{0};
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Verify it compiles**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/sched/a2ws.hpp -o /dev/null 2>&1
# Expected: no errors
```

---

### Task 3.4: Create `src/sched/a2ws.cpp`

**Files:**
- Create: `src/sched/a2ws.cpp`
- Depends on: Task 3.3

**Tasks:**
- [ ] **Step 1: Write implementation**

```cpp
// src/sched/a2ws.cpp
#include <hpactor/sched/a2ws.hpp>
#include <algorithm>

namespace hpactor::sched {

A2WSCoordinator::A2WSCoordinator(size_t num_workers, uint32_t steal_threshold)
    : num_workers_(num_workers), threshold_(steal_threshold),
      snapshots_(num_workers) {}

void A2WSCoordinator::update_snapshot(size_t worker_index, uint32_t load) noexcept {
    snapshots_[worker_index].load.store(load, std::memory_order_relaxed);
}

size_t A2WSCoordinator::best_victim(size_t requester, uint8_t /*priority*/) const noexcept {
    if (num_workers_ < 2) return SIZE_MAX;

    size_t left  = (requester + num_workers_ - 1) % num_workers_;
    size_t right = (requester + 1) % num_workers_;

    uint32_t left_load  = snapshots_[left].load.load(std::memory_order_relaxed);
    uint32_t right_load = snapshots_[right].load.load(std::memory_order_relaxed);
    uint32_t own_load   = snapshots_[requester].load.load(std::memory_order_relaxed);

    // Check threshold
    size_t  victim;
    uint32_t victim_load;
    if (left_load > right_load) {
        victim = left; victim_load = left_load;
    } else {
        victim = right; victim_load = right_load;
    }

    if (victim_load - own_load <= threshold_) {
        return SIZE_MAX;  // imbalance below threshold
    }

    return victim;
}

uint32_t A2WSCoordinator::steal_count(uint32_t own_load, uint32_t victim_load) const noexcept {
    int diff = static_cast<int>(victim_load) - static_cast<int>(own_load);
    if (diff <= 0) return 1;
    return static_cast<uint32_t>(diff / 2);
}

bool A2WSCoordinator::is_low_load() const noexcept {
    uint32_t total = 0;
    for (const auto& snap : snapshots_) {
        total += snap.load.load(std::memory_order_relaxed);
    }
    return total < 2 * num_workers_;  // α = 2
}

size_t A2WSCoordinator::max_concurrent_stealers() const noexcept {
    return num_workers_ / 4;
}

} // namespace hpactor::sched
```

- [ ] **Step 2: Verify and commit**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c src/sched/a2ws.cpp -o /dev/null 2>&1
git add include/hpactor/sched/a2ws.hpp src/sched/a2ws.cpp
git commit -m "feat(sched): add A2WSCoordinator for adaptive work-stealing

Ring-topology load snapshots with threshold-based victim selection.
best_victim() checks neighbors' load imbalance and returns the better
victim or SIZE_MAX if below threshold.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 3.5: Upgrade `HybridScheduler` to use EDF + A2WS

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp`, `src/sched/scheduler.cpp`
- Depends on: Tasks 3.1, 3.4

**Tasks:**
- [ ] **Step 1: Update `HybridScheduler` declaration**

In `scheduler.hpp`, add members:
```cpp
EDFPriorityQueue               edf_queue_;
A2WSCoordinator                a2ws_;
```

And upgrade `select_worker`:
```cpp
// Select worker using A2WS (Phase 3) instead of round-robin (Phase 2)
size_t select_worker(uint8_t priority) const override;
```

- [ ] **Step 2: Update `notify_ready` implementation**

```cpp
void HybridScheduler::notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) {
    if (!running_.load(std::memory_order_acquire)) return;

    size_t worker_idx = select_worker(priority);
    edf_queue_.upsert({actor, priority, deadline_ns, worker_idx});
    WorkItem item{actor, deadline_ns, next_sequence_.fetch_add(1, std::memory_order_relaxed)};
    workers_[worker_idx].push(item);
}
```

- [ ] **Step 3: Implement A2WS-aware `select_worker`**

```cpp
size_t HybridScheduler::select_worker(uint8_t priority) const {
    // A2WS: check ring neighbors
    // requester index = 0 for simplicity (Phase 3 full per-worker calling)
    // For Phase 3: each worker calls this; use requester=worker_index
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < workers_.size(); ++i) {
        size_t victim = a2ws_.best_victim(i, priority);
        if (victim != SIZE_MAX) {
            best = victim;
            break;  // take first worker with a stealable neighbor
        }
    }
    if (best == SIZE_MAX) {
        // Fall back to round-robin
        size_t idx = priority % workers_.size();
        best = rr_counters_[idx].fetch_add(1, std::memory_order_relaxed) % workers_.size();
    }
    return best;
}
```

- [ ] **Step 4: Update `notify_idle` to remove from EDF**

```cpp
void HybridScheduler::notify_idle(ActorId actor) {
    edf_queue_.remove(actor);
}
```

- [ ] **Step 5: Verify build and run all Phase 1-2 tests**

```bash
ninja -C build 2>&1 | tail -10
ctest --output-on-failure -R "chaselev|multi_priority|hybrid" 2>&1
# Expected: all pass
```

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp
git commit -m "feat(sched): integrate EDFPriorityQueue and A2WS into HybridScheduler

notify_ready now upserts to EDF queue before routing to worker.
notify_idle removes from EDF queue. select_worker upgraded to check
A2WS neighbors before falling back to round-robin.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 3.6: Create Phase 3 EDF + A2WS test

**Files:**
- Create: `tests/sched/test_edf_a2ws.cpp`
- Depends on: Task 3.5

**Tasks:**
- [ ] **Step 1: Write EDF + A2WS unit tests**

```cpp
// tests/sched/test_edf_a2ws.cpp
#include <cassert>
#include <hpactor/sched/edf_queue.hpp>
#include <hpactor/sched/a2ws.hpp>
#include <thread>
#include <vector>

int main() {
    // --- EDFPriorityQueue tests ---
    hpactor::sched::EDFPriorityQueue q;

    // Test: empty returns false
    hpactor::sched::EDFPriorityQueue::EDFEntry out;
    assert(!q.pop_min(out));

    // Test: insert two actors, earliest deadline returns first
    q.upsert({hpactor::ActorId{1}, 0, 1000, 0});  // deadline 1000
    q.upsert({hpactor::ActorId{2}, 0, 500, 0});   // deadline 500 (earlier)
    q.upsert({hpactor::ActorId{3}, 0, 1500, 0});  // deadline 1500

    assert(q.pop_min(out));
    assert(out.actor.value() == 2);   // earliest deadline
    assert(q.pop_min(out));
    assert(out.actor.value() == 1);  // next
    assert(q.pop_min(out));
    assert(out.actor.value() == 3);  // last

    // Test: same deadline, lower priority returns first
    hpactor::sched::EDFPriorityQueue q2;
    q2.upsert({hpactor::ActorId{1}, 2, 1000, 0});  // priority 2
    q2.upsert({hpactor::ActorId{2}, 1, 1000, 0});  // priority 1 (higher)
    assert(q2.pop_min(out));
    assert(out.actor.value() == 2);   // priority 1 is higher
    assert(q2.pop_min(out));
    assert(out.actor.value() == 1);

    // Test: remove mid-heap
    hpactor::sched::EDFPriorityQueue q3;
    q3.upsert({hpactor::ActorId{1}, 0, 1000, 0});
    q3.upsert({hpactor::ActorId{2}, 0, 500, 0});
    q3.upsert({hpactor::ActorId{3}, 0, 1500, 0});
    q3.remove(hpactor::ActorId{2});  // remove middle element
    assert(q3.size() == 2);
    q3.pop_min(out);
    assert(out.actor.value() == 1);  // 500 was removed; next is 1000

    // --- A2WSCoordinator tests ---
    hpactor::sched::A2WSCoordinator a2ws(4, 2);  // 4 workers, threshold=2

    // Test: best_victim returns SIZE_MAX when load is balanced
    assert(a2ws.best_victim(0, 0) == SIZE_MAX);  // all zero

    // Test: imbalanced load triggers steal
    a2ws.update_snapshot(0, 10);   // worker 0 has 10 tasks
    a2ws.update_snapshot(1, 0);   // worker 1 has 0
    a2ws.update_snapshot(2, 0);
    a2ws.update_snapshot(3, 0);
    assert(a2ws.best_victim(1, 0) == 0);  // worker 1 should steal from worker 0

    // Test: steal_count halving heuristic
    assert(a2ws.steal_count(0, 10) == 5);  // (10-0)/2 = 5
    assert(a2ws.steal_count(0, 3) == 1);   // (3-0)/2 = 1 (min 1)

    return 0;
}
```

- [ ] **Step 2: Build and run**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include tests/sched/test_edf_a2ws.cpp src/sched/edf_queue.cpp src/sched/a2ws.cpp -o build/test_edf_a2ws 2>&1
./build/test_edf_a2ws; echo "Exit: $?"
# Expected: 0
```

- [ ] **Step 3: Commit**

```bash
git add tests/sched/test_edf_a2ws.cpp
git commit -m "test(sched): add EDFPriorityQueue and A2WS unit tests

EDF tests verify earliest-deadline-first ordering and priority tie-breaking.
A2WS tests verify threshold-based victim selection and steal_count halving.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 4: Hierarchical Timing Wheel

### Task 4.1: Create `include/hpactor/timer/timing_wheel.hpp`

**Files:**
- Create: `include/hpactor/timer/timing_wheel.hpp`
- Create: `include/hpactor/timer/timer_entry.hpp`
- Depends on: Phase 3 complete

**Tasks:**
- [ ] **Step 1: Write `timer_entry.hpp`**

```cpp
// include/hpactor/timer/timer_entry.hpp
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace hpactor::timer {

using timer_callback = std::function<void()>;

struct TimerHandle {
    uint64_t id = 0;
    bool valid() const noexcept { return id != 0; }
};

// Intrusive timer entry. Callers embed this in their own structures.
// Zero extra allocation required.
struct TimerEntry {
    TimerHandle handle;
    int64_t     expires_at_ns;   // absolute CLOCK_MONOTONIC
    int64_t     interval_ns;     // 0 = one-shot
    timer_callback cb;
    TimerEntry*  next = nullptr;  // intrusive list link within a slot
};

} // namespace hpactor::timer
```

- [ ] **Step 2: Write `timing_wheel.hpp`**

```cpp
// include/hpactor/timer/timing_wheel.hpp
#pragma once

#include "timer_entry.hpp"
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace hpactor::timer {

// W-level hierarchical timing wheel for O(1) amortized timer insert/cancel.
// All timer operations run on a single dedicated advancing thread.
class HierarchicalTimingWheel {
public:
    struct Config {
        uint32_t levels         = 4;
        uint32_t base_slots     = 256;          // S₀
        int64_t  base_tick_ns   = 1'000'000;     // 1 ms base resolution
    };

    explicit HierarchicalTimingWheel(Config cfg = {});
    ~HierarchicalTimingWheel();

    // Insert a one-shot timer firing after `delay_ns` nanoseconds.
    TimerHandle insert_after(int64_t delay_ns, timer_callback cb);

    // Insert a repeating timer every `interval_ns` nanoseconds.
    TimerHandle insert_every(int64_t interval_ns, timer_callback cb);

    // Cancel a pending timer. Safe after the timer has already fired.
    void cancel(TimerHandle handle);

    // Advance the wheel to `now_ns`. Must be called from the advancing thread.
    // Returns the number of timers that fired.
    int advance(int64_t now_ns);

    // Time of next scheduled expiry, or INT64_MAX if no timers pending.
    int64_t next_expiry_ns() const;

private:
    void place(TimerEntry* entry, int64_t expiry_ns, int64_t delay_ns);
    void cascade(size_t level, uint32_t slot);
    int fire_expired(int64_t now_ns);

    struct Slot {
        TimerEntry* head = nullptr;  // intrusive singly-linked list
    };

    struct Wheel {
        std::vector<Slot>  slots;
        uint32_t           current = 0;
        int64_t            resolution_ns;
    };

    Config                              cfg_;
    std::vector<Wheel>                  wheels_;
    std::unordered_map<uint64_t, TimerEntry*> entries_;  // handle.id → entry
    uint64_t                            next_id_{1};
    int64_t                             base_tick_ns_;
};

} // namespace hpactor::timer
```

- [ ] **Step 3: Verify both headers compile**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/timer/timer_entry.hpp -o /dev/null 2>&1
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/timer/timing_wheel.hpp -o /dev/null 2>&1
# Expected: no errors
```

---

### Task 4.2: Create `src/timer/timing_wheel.cpp`

**Files:**
- Create: `src/timer/timing_wheel.cpp`
- Depends on: Task 4.1

**Tasks:**
- [ ] **Step 1: Write implementation (full) — with `expires_at_ns` computed correctly**

```cpp
// src/timer/timing_wheel.cpp
#include <hpactor/timer/timing_wheel.hpp>
#include <chrono>
#include <cassert>
#include <climits>

namespace hpactor::timer {

inline int64_t now_ns() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

HierarchicalTimingWheel::HierarchicalTimingWheel(Config cfg)
    : cfg_(cfg), entries_(), next_id_(1), base_tick_ns_(cfg.base_tick_ns) {
    int64_t resolution = cfg.base_tick_ns;
    for (uint32_t k = 0; k < cfg.levels; ++k) {
        Wheel w;
        uint32_t num_slots = (k == 0) ? cfg.base_slots : 64;
        w.slots.resize(num_slots);
        w.resolution_ns = resolution;
        wheels_.push_back(std::move(w));
        resolution *= num_slots;
    }
}

HierarchicalTimingWheel::~HierarchicalTimingWheel() = default;

TimerHandle HierarchicalTimingWheel::insert_after(int64_t delay_ns, timer_callback cb) {
    int64_t expiry = now_ns() + delay_ns;
    auto* entry = new TimerEntry{
        .handle        = TimerHandle{next_id_.fetch_add(1)},
        .expires_at_ns = expiry,
        .interval_ns   = 0,
        .cb            = std::move(cb),
        .next          = nullptr
    };
    entries_[entry->handle.id] = entry;
    place(entry, expiry, delay_ns);
    return entry->handle;
}

TimerHandle HierarchicalTimingWheel::insert_every(int64_t interval_ns, timer_callback cb) {
    int64_t expiry = now_ns() + interval_ns;
    auto* entry = new TimerEntry{
        .handle        = TimerHandle{next_id_.fetch_add(1)},
        .expires_at_ns = expiry,
        .interval_ns   = interval_ns,
        .cb            = std::move(cb),
        .next          = nullptr
    };
    entries_[entry->handle.id] = entry;
    place(entry, expiry, interval_ns);
    return entry->handle;
}

void HierarchicalTimingWheel::cancel(TimerHandle handle) {
    auto it = entries_.find(handle.id);
    if (it != entries_.end()) {
        it->second->cb = nullptr;  // mark cancelled — skip at fire time
    }
}

int HierarchicalTimingWheel::advance(int64_t now_ns) {
    return fire_expired(now_ns);
}

int64_t HierarchicalTimingWheel::next_expiry_ns() const {
    int64_t min_expiry = INT64_MAX;
    for (const auto& [id, entry] : entries_) {
        if (entry->cb && entry->expires_at_ns < min_expiry) {
            min_expiry = entry->expires_at_ns;
        }
    }
    return min_expiry;
}

void HierarchicalTimingWheel::place(TimerEntry* entry, int64_t expiry_ns, int64_t delay_ns) {
    // Determine target level based on delay_ns
    int64_t remaining = delay_ns;
    size_t  target_level = 0;
    for (size_t k = 0; k < wheels_.size(); ++k) {
        int64_t level_span = static_cast<int64_t>(wheels_[k].slots.size())
                             * wheels_[k].resolution_ns;
        if (remaining < level_span) {
            target_level = k;
            break;
        }
        if (k == wheels_.size() - 1) {
            target_level = k;
        }
    }

    Wheel& w = wheels_[target_level];
    // Slot = (current + delay / resolution) mod slot_count
    uint32_t slot = static_cast<uint32_t>(
        (w.current + remaining / w.resolution_ns) % w.slots.size()
    );

    entry->next = w.slots[slot].head;
    w.slots_[slot].head = entry;
    (void)expiry_ns;  // expiry_ns stored in entry->expires_at_ns by caller
}

int HierarchicalTimingWheel::fire_expired(int64_t now_ns) {
    int fired = 0;
    Wheel& w0 = wheels_[0];

    uint32_t slot = w0.current;
    TimerEntry** prev = &w0.slots_[slot].head;
    while (*prev) {
        TimerEntry* entry = *prev;
        if (entry->expires_at_ns <= now_ns) {
            *prev = entry->next;
            if (entry->cb) {
                entry->cb();
                ++fired;
            }
            if (entry->interval_ns > 0) {
                // Reschedule repeating timer
                entry->expires_at_ns = now_ns + entry->interval_ns;
                place(entry, entry->expires_at_ns, entry->interval_ns);
            } else {
                delete entry;
                entries_.erase(entry->handle.id);
            }
        } else {
            prev = &entry->next;
        }
    }

    w0.current = (w0.current + 1) % w0.slots_[0].size();
    return fired;
}

} // namespace hpactor::timer
```

> **Critical:** `expires_at_ns` must be set **before** calling `place()` in both `insert_after` and `insert_every`. The `place()` function uses `delay_ns` to compute the slot, not `expiry_ns`. This avoids the logic bug where `fire_expired` would incorrectly fire all timers because `expires_at_ns == 0`.

- [ ] **Step 2: Verify it compiles and the bug is absent**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c src/timer/timing_wheel.cpp -o /dev/null 2>&1
# Expected: no errors
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/timer/timing_wheel.hpp include/hpactor/timer/timer_entry.hpp src/timer/timing_wheel.cpp
git commit -m "feat(timer): add HierarchicalTimingWheel with O(1) amortized insert/cancel

4-level wheel (ms/s/min/hr). insert_after/insert_every compute absolute
expiry before place(). Repeating timers reschedule via place() after firing.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 4.3: Integrate timing wheel into `HybridScheduler`

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp`, `src/sched/scheduler.cpp`
- Depends on: Task 4.2

**Tasks:**
- [ ] **Step 1: Add timer wheel member and dedicated thread to `HybridScheduler`**

```cpp
// In scheduler.hpp, add:
#include <hpactor/timer/timing_wheel.hpp>
// In HybridScheduler private members:
timer::HierarchicalTimingWheel       timer_wheel_;
std::thread                          timer_thread_;
std::atomic<bool>                    timer_running_{false};

// Update schedule_after/schedule_every implementations to use timer_wheel_
```

- [ ] **Step 2: Start timer thread in `start()`**

```cpp
void HybridScheduler::start() {
    running_.store(true, std::memory_order_release);
    for (auto& w : workers_) { w.start(); }
    timer_running_.store(true, std::memory_order_release);
    timer_thread_ = std::thread([this] {
        while (timer_running_.load(std::memory_order_acquire)) {
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            int fired = timer_wheel_.advance(now);
            auto next = timer_wheel_.next_expiry_ns();
            // Sleep until next tick or timer_wheel_.base_tick_ns_, whichever is sooner
            int64_t sleep_ns = std::min<int64_t>(next - now, timer_wheel_.base_tick_ns());
            std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
        }
    });
}
```

- [ ] **Step 3: Stop timer thread in `stop()`**

```cpp
void HybridScheduler::stop() {
    timer_running_.store(false, std::memory_order_release);
    if (timer_thread_.joinable()) timer_thread_.join();
    running_.store(false, std::memory_order_release);
    for (auto& w : workers_) { w.stop(); }
}
```

- [ ] **Step 4: Wire timer methods**

```cpp
TimerHandle HybridScheduler::schedule_after(timer_callback cb, int64_t delay_ns) {
    return timer_wheel_.insert_after(delay_ns, std::move(cb));
}
TimerHandle HybridScheduler::schedule_every(timer_callback cb, int64_t interval_ns) {
    return timer_wheel_.insert_every(interval_ns, std::move(cb));
}
void HybridScheduler::cancel_timer(TimerHandle handle) {
    timer_wheel_.cancel(handle);
}
```

- [ ] **Step 5: Build and test**

```bash
ninja -C build 2>&1 | tail -10
```

---

## Phase 5: Coroutine Frame Pool

### Task 5.1: Create `include/hpactor/memory/coroutine_frame_pool.hpp`

**Files:**
- Create: `include/hpactor/memory/coroutine_frame_pool.hpp`
- Depends on: Phase 4 complete

**Tasks:**
- [ ] **Step 1: Write header**

```cpp
// include/hpactor/memory/coroutine_frame_pool.hpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace hpactor::memory {

// Thread-local slab pool for C++20 coroutine frames.
// Must only be accessed from the owning thread.
class CoroutineFramePool {
public:
    static constexpr size_t DEFAULT_SLOT_SIZE  = 512;
    static constexpr size_t DEFAULT_SLOT_COUNT = 256;

    explicit CoroutineFramePool(
        size_t slot_size  = DEFAULT_SLOT_SIZE,
        size_t slot_count = DEFAULT_SLOT_COUNT);
    ~CoroutineFramePool();

    // Allocate a slot of at least `size` bytes.
    // O(1): bitmap TZCNT + conditional store.
    // Falls back to global heap if size > slot_size_ or pool is exhausted.
    [[nodiscard]] void* allocate(size_t size) noexcept;

    // Release a slot returned by allocate().
    void deallocate(void* ptr) noexcept;

    // Diagnostics
    size_t hits()     const noexcept { return hits_; }
    size_t misses()   const noexcept { return misses_; }  // fell back to heap
    size_t capacity() const noexcept { return slot_count_; }

private:
    size_t                      slot_size_;
    size_t                      slot_count_;
    std::unique_ptr<uint8_t[]>  storage_;    // slot_count_ * slot_size_ bytes
    std::vector<uint64_t>       bitmap_;     // 1 bit per slot; 1 = free
    size_t                      hits_{0};
    size_t                      misses_{0};
};

} // namespace hpactor::memory
```

- [ ] **Step 2: Verify it compiles**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/memory/coroutine_frame_pool.hpp -o /dev/null 2>&1
# Expected: no errors
```

---

### Task 5.2: Create `src/memory/coroutine_frame_pool.cpp`

**Files:**
- Create: `src/memory/coroutine_frame_pool.cpp`
- Depends on: Task 5.1

**Tasks:**
- [ ] **Step 1: Write implementation**

```cpp
// src/memory/coroutine_frame_pool.cpp
#include <hpactor/memory/coroutine_frame_pool.hpp>
#include <cstring>

namespace hpactor::memory {

CoroutineFramePool::CoroutineFramePool(size_t slot_size, size_t slot_count)
    : slot_size_(slot_size), slot_count_(slot_count),
      storage_(new uint8_t[slot_count * slot_size]),
      bitmap_((slot_count + 63) / 64, ~0ULL) {  // all slots initially free
}

CoroutineFramePool::~CoroutineFramePool() = default;

void* CoroutineFramePool::allocate(size_t size) noexcept {
    if (size > slot_size_) {
        ++misses_;
        return ::operator new(size);
    }

    for (size_t word = 0; word < bitmap_.size(); ++word) {
        uint64_t w = bitmap_[word];
        if (w == 0) continue;
        int tz = __builtin_ctzll(w);  // index of first free slot
        bitmap_[word] &= ~(1ULL << tz);
        size_t slot_idx = word * 64 + tz;
        if (slot_idx >= slot_count_) {
            bitmap_[word] |= (1ULL << tz);  // put it back
            continue;
        }
        ++hits_;
        return storage_.get() + slot_idx * slot_size_;
    }

    // Pool exhausted
    ++misses_;
    return ::operator new(size);
}

void CoroutineFramePool::deallocate(void* ptr) noexcept {
    if (!ptr) return;

    // Check if ptr is in our storage
    auto* base = storage_.get();
    if (ptr >= base && ptr < base + slot_count_ * slot_size_) {
        size_t offset = static_cast<uint8_t*>(ptr) - base;
        size_t slot_idx = offset / slot_size_;
        size_t word = slot_idx / 64;
        size_t bit = slot_idx % 64;
        bitmap_[word] |= (1ULL << bit);  // mark free
    } else {
        ::operator delete(ptr);  // was a heap allocation
    }
}

} // namespace hpactor::memory
```

- [ ] **Step 2: Verify and commit**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c src/memory/coroutine_frame_pool.cpp -o /dev/null 2>&1
git add include/hpactor/memory/coroutine_frame_pool.hpp src/memory/coroutine_frame_pool.cpp
git commit -m "feat(memory): add thread-local coroutine frame slab pool

CoroutineFramePool provides O(1) bitmap-based slot allocation for
C++20 stackless coroutine frames. Falls back to global heap for
frames exceeding slot_size. Phase 5 wires this into WorkerThread.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 5.3: Wire frame pool into `WorkerThread`

**Files:**
- Modify: `include/hpactor/sched/worker_thread.hpp`, `src/sched/worker_thread.cpp`
- Depends on: Task 5.2

**Tasks:**
- [ ] **Step 1: Add `CoroutineFramePool` member and accessor to `WorkerThread`**

```cpp
// In worker_thread.hpp, add:
#include <hpactor/memory/coroutine_frame_pool.hpp>
// Add member:
memory::CoroutineFramePool              frame_pool_{512, 256};
// Add accessor:
memory::CoroutineFramePool& frame_pool() { return frame_pool_; }
```

- [ ] **Step 2: Expose `CoroutineFramePool` allocation to coroutine `promise_type`**

**Where to add the interception:** In `include/hpactor/actor/event_based_actor.hpp`, the `EventBasedActor` class has a nested `promise_type` struct that defines `get_return_object()`, `initial_suspend()`, etc. Add the thread-local pointer and `operator new`/`operator delete` overrides there:

```cpp
// In include/hpactor/actor/event_based_actor.hpp

// Forward-declare CoroutineFramePool
namespace hpactor::memory { class CoroutineFramePool; }

// Thread-local pointer to the current worker's frame pool
inline thread_local memory::CoroutineFramePool* tl_frame_pool = nullptr;

struct EventBasedActor::promise_type {
    // ... existing methods ...

    static void* operator new(size_t sz) {
        if (tl_frame_pool) {
            void* ptr = tl_frame_pool->allocate(sz);
            if (ptr) return ptr;
        }
        return ::operator new(sz);
    }

    static void operator delete(void* ptr) noexcept {
        if (tl_frame_pool) tl_frame_pool->deallocate(ptr);
        else ::operator delete(ptr);
    }
};
```

The thread-local `tl_frame_pool` is set by `WorkerThread::start()`:

```cpp
// In src/sched/worker_thread.cpp
void WorkerThread::start() {
    tl_frame_pool = &frame_pool_;  // expose pool to coroutine allocations
    thread_ = std::thread([this] { run_loop(); });
}
```

> **Why this location?** `promise_type` is a nested struct of `EventBasedActor`. Adding `operator new`/`operator delete` overrides here intercepts all coroutine frame allocations for every actor type that uses `EventBasedActor` as a base — which covers all event-based actors in the system.

- [ ] **Step 3: Verify full build**

```bash
ninja -C build 2>&1 | tail -10
ctest --output-on-failure 2>&1 | tail -20
# Expected: all tests pass
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/sched/worker_thread.hpp src/sched/worker_thread.cpp
git commit -m "feat(sched): wire CoroutineFramePool into WorkerThread

Each worker thread now owns a thread-local slab allocator for coroutine
frames. promise_type::operator new intercepts allocations via a
thread_local pointer set on worker start.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Dependency Graph

```
Phase 1 ──┬── 1.1 (test) ── 1.2 (header) ── 1.3 (.cpp) ── 1.4 (test) ── 1.5 (cmake)
Phase 2 ──┬── 2.1 (scheduler.hpp) ── 2.2 (worker_thread.hpp)
          ├──── 2.3 (worker_thread.cpp) ── 2.4 (scheduler.cpp) ── 2.5 (ActorSystem)
          └──── 2.6 (integration test)
Phase 3 ──┬── 3.1 (edf_queue.hpp) ── 3.2 (edf_queue.cpp)
          ├──── 3.3 (a2ws.hpp) ── 3.4 (a2ws.cpp)
          └──── 3.5 (HybridScheduler upgrade) ── 3.6 (EDF+A2WS test)
Phase 4 ──┬── 4.1 (timing_wheel.hpp) ── 4.2 (timing_wheel.cpp) ── 4.3 (integration)
Phase 5 ──┬── 5.1 (frame_pool.hpp) ── 5.2 (frame_pool.cpp) ── 5.3 (WorkerThread wire)
```

---

## Verification Checklist

After each phase:
- [ ] Phase 1: `test_chaselev_deque` and `test_multi_priority_work_queue` pass
- [ ] Phase 2: `test_hybrid_scheduler` passes, actor send/receive works
- [ ] Phase 3: EDF queue orders by deadline; A2WS victim selection fires
- [ ] Phase 4: `schedule_after` fires within 1ms of deadline; cancel is no-op
- [ ] Phase 5: coroutine frame allocated from pool (verified via `hits()` / `misses()`)
- [ ] Full build: `ninja -C build` with no errors
- [ ] Full tests: `ctest --output-on-failure` all pass
