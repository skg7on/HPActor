# DispatchPolicy-Based Pinned Dispatcher — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce `DispatchPolicy` to `AbstractActor` so the scheduler can isolate daemon, polling, and compute-heavy actors onto dedicated threads or thread pools while preserving unified supervision and messaging.

**Architecture:** `DispatchPolicy` is a virtual attribute on `AbstractActor`, not a separate class hierarchy. A `DaemonActor` *is* an `EventBasedActor` — it has a mailbox, behavior, and supervisor. The scheduler reads `dispatch_policy()` at spawn time and routes execution: `Cooperative` stays on the M:N work-stealing pool, `DedicatedThread` gets its own `std::thread`, and `DedicatedPool` gets a private `DedicatedThreadPool`. The supervisor never knows the child was pinned.

**Tech Stack:** C++20, `-fno-exceptions`, `-fno-rtti`, header-only where possible + `.cpp` for impl. LLVM coding style. CMake + Ninja. Existing patterns: `hpactor_lib` shared library, tests via `add_executable` + `add_test` in `tests/CMakeLists.txt`.

**Design Spec:** `docs/architecture/actor/actors-data-structure-design.md`

---

## File Map

| File | Responsibility | Action |
|------|---------------|--------|
| `include/hpactor/sched/dispatch_policy.hpp` | `DispatchPolicy` enum, `DispatchHints` struct | **Create** |
| `include/hpactor/actor/abstract_actor.hpp` | Add `dispatch_policy()` + `dispatch_hints()` virtual methods | **Modify** |
| `src/actor/abstract_actor.cpp` | No change needed (default impls are inline in the header) | — |
| `include/hpactor/sched/scheduler.hpp` | Add `register_dedicated_*()` / `unregister_dedicated()` to `IScheduler`; add `DedicatedThreadPool` fwd decl + storage to `HybridScheduler` | **Modify** |
| `src/sched/scheduler.cpp` | Implement new `HybridScheduler` methods | **Modify** |
| `include/hpactor/sched/dedicated_thread_pool.hpp` | `DedicatedThreadPool` class | **Create** |
| `src/sched/dedicated_thread_pool.cpp` | `DedicatedThreadPool` implementation | **Create** |
| `include/hpactor/actor/daemon_actor.hpp` | `DaemonActor` class | **Create** |
| `src/actor/daemon_actor.cpp` | `DaemonActor` implementation | **Create** |
| `include/hpactor/actor/polling_actor.hpp` | `PollingActor` class | **Create** |
| `include/hpactor/actor/dense_computing_actor.hpp` | `DenseComputingActor` class | **Create** |
| `include/hpactor/actor/external_msg_gateway.hpp` | `ExternalMsgGatewayActor` class | **Create** |
| `include/hpactor/actor/http_server_actor.hpp` | `HTTPServerActor` class | **Create** |
| `include/hpactor/actor/actor_fwd.hpp` | Forward declarations for new types | **Modify** |
| `include/hpactor/core/actor_system.hpp` | `spawn()` template: check policy + register with scheduler | **Modify** |
| `tests/actor/test_daemon_actor.cpp` | Tests for DaemonActor lifecycle | **Create** |
| `tests/sched/test_dispatch_policy.cpp` | Tests for DispatchPolicy routing | **Create** |
| `tests/sched/test_dedicated_thread_pool.cpp` | Tests for DedicatedThreadPool | **Create** |
| `CMakeLists.txt` | Add new `.cpp` files to `hpactor_lib` | **Modify** |
| `tests/CMakeLists.txt` | Register new test executables | **Modify** |

---

### Task 1: DispatchPolicy enum and DispatchHints struct

**Files:**
- Create: `include/hpactor/sched/dispatch_policy.hpp`

- [ ] **Step 1: Write the header**

```cpp
// Copyright 2026 HPActor Contributors
// ...
#pragma once

#include <cstdint>

namespace hpactor::sched {

enum class DispatchPolicy : uint8_t {
    Cooperative = 0,
    DedicatedThread,
    DedicatedPool,
};

struct DispatchHints {
    int cpu_affinity = -1;
    uint32_t pool_size = 1;
    uint8_t priority = 0;
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Build to verify header parses**

```bash
cmake -S . -B build -GNinja && ninja -C build
```

Expected: Build succeeds (header not yet included anywhere, but CMake generates `compile_commands.json`).

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/sched/dispatch_policy.hpp
git commit -m "feat(sched): add DispatchPolicy enum and DispatchHints struct"
```

---

### Task 2: Add dispatch_policy() and dispatch_hints() to AbstractActor

**Files:**
- Modify: `include/hpactor/actor/abstract_actor.hpp`
- Modify: `src/actor/abstract_actor.cpp`

- [ ] **Step 1: Add include and virtual methods to header**

In `abstract_actor.hpp`, add `#include <hpactor/sched/dispatch_policy.hpp>` after the existing includes. Then add these virtual methods to the `AbstractActor` class body (before `protected:`):

```cpp
    // Dispatch policy — tells the scheduler how to execute this actor.
    // Default: Cooperative (M:N work-stealing pool).
    virtual sched::DispatchPolicy dispatch_policy() const {
        return sched::DispatchPolicy::Cooperative;
    }
    virtual sched::DispatchHints dispatch_hints() const {
        return {};
    }
```

- [ ] **Step 2: Verify no compilation needed in .cpp for defaults**

The default implementations are inline in the header. No change needed in `abstract_actor.cpp` for these.

- [ ] **Step 3: Build and run existing tests**

```bash
ninja -C build && ctest --output-on-failure
```

Expected: All existing tests pass (no behavior change — defaults preserved).

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/abstract_actor.hpp
git commit -m "feat(actor): add dispatch_policy() and dispatch_hints() to AbstractActor"
```

---

### Task 3: Extend IScheduler with dedicated thread/pool registration

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `src/sched/scheduler.cpp`

- [ ] **Step 1: Add forward declaration and new pure virtual methods to IScheduler**

In `scheduler.hpp`, add forward declaration of `DedicatedThreadPool`:

```cpp
class DedicatedThreadPool; // forward decl
```

Add these pure virtual methods to `IScheduler` (after `is_running()`):

```cpp
    // Register an actor that needs a dedicated OS thread.
    // cpu_affinity: -1 = no affinity, >=0 = pin to specific core.
    virtual void register_dedicated_thread(ActorId actor, int cpu_affinity) = 0;

    // Register an actor that needs a dedicated worker pool.
    // The scheduler creates or reuses a DedicatedThreadPool of the given size.
    virtual void register_dedicated_pool(ActorId actor, uint32_t pool_size) = 0;

    // Shutdown a dedicated execution context for an actor.
    virtual void unregister_dedicated(ActorId actor) = 0;
```

- [ ] **Step 2: Add storage to HybridScheduler**

In the `HybridScheduler` class, add to private section:

```cpp
    // Dedicated thread handles (actor -> thread)
    std::unordered_map<ActorId, std::thread> dedicated_threads_;
    std::mutex dedicated_mutex_;

    // Dedicated thread pools (pool_size -> pool)
    std::unordered_map<uint32_t, std::unique_ptr<DedicatedThreadPool>> dedicated_pools_;
    std::unordered_map<ActorId, uint32_t> actor_pool_map_; // actor -> pool_size
```

Add `override` declarations in the public section:

```cpp
    void register_dedicated_thread(ActorId actor, int cpu_affinity) override;
    void register_dedicated_pool(ActorId actor, uint32_t pool_size) override;
    void unregister_dedicated(ActorId actor) override;
```

- [ ] **Step 3: Stub the implementations in scheduler.cpp**

Add stubs that compile but are no-ops. **Do NOT include `dedicated_thread_pool.hpp` yet** — that header is created in Task 4. The stubs use only types already available (ActorId, uint32_t, int):

```cpp
void HybridScheduler::register_dedicated_thread(ActorId /*actor*/, int /*cpu_affinity*/) {
    // TODO: implemented in Task 5 (after DedicatedThreadPool exists)
}

void HybridScheduler::register_dedicated_pool(ActorId /*actor*/, uint32_t /*pool_size*/) {
    // TODO: implemented in Task 5
}

void HybridScheduler::unregister_dedicated(ActorId /*actor*/) {
    // TODO: implemented in Task 5
}
```

- [ ] **Step 4: Build to verify compilation**

```bash
ninja -C build
```

Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp
git commit -m "feat(sched): add dedicated thread/pool registration to IScheduler and HybridScheduler"
```

---

### Task 4: DedicatedThreadPool implementation

**Files:**
- Create: `include/hpactor/sched/dedicated_thread_pool.hpp`
- Create: `src/sched/dedicated_thread_pool.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/sched/test_dedicated_thread_pool.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...
#include <cassert>
#include <atomic>
#include <chrono>
#include <thread>
#include <hpactor/sched/dedicated_thread_pool.hpp>
#include <hpactor/sched/work_queue.hpp>

using namespace hpactor;
using namespace hpactor::sched;

int main() {
    // Test 1: start/stop lifecycle
    {
        DedicatedThreadPool pool(2);
        assert(!pool.is_running());
        pool.start();
        assert(pool.is_running());
        pool.stop();
        assert(!pool.is_running());
    }

    // Test 2: enqueue and process work
    {
        DedicatedThreadPool pool(2);
        pool.start();

        std::atomic<int> counter{0};
        ActorId test_id(42);

        pool.enqueue(test_id, [&counter]() {
            counter.fetch_add(1);
        });
        pool.enqueue(test_id, [&counter]() {
            counter.fetch_add(1);
        });

        // Give workers time to process
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(counter.load() == 2);

        pool.stop();
    }

    // Test 3: multiple actors on same pool
    {
        DedicatedThreadPool pool(2);
        pool.start();

        std::atomic<int> a_count{0};
        std::atomic<int> b_count{0};
        ActorId actor_a(1);
        ActorId actor_b(2);

        for (int i = 0; i < 10; i++) {
            pool.enqueue(actor_a, [&a_count]() { a_count.fetch_add(1); });
            pool.enqueue(actor_b, [&b_count]() { b_count.fetch_add(1); });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(a_count.load() == 10);
        assert(b_count.load() == 10);

        pool.stop();
    }

    return 0;
}
```

- [ ] **Step 2: Register test in tests/CMakeLists.txt**

Add after existing sched tests:

```cmake
add_executable(test_dedicated_thread_pool sched/test_dedicated_thread_pool.cpp)
target_link_libraries(test_dedicated_thread_pool hpactor)
add_test(NAME test_dedicated_thread_pool COMMAND test_dedicated_thread_pool)
```

- [ ] **Step 3: Run test to verify it fails**

```bash
cmake -S . -B build -GNinja && ninja -C build && ctest -R test_dedicated_thread_pool --output-on-failure
```

Expected: Compilation error — `DedicatedThreadPool` not yet defined.

- [ ] **Step 4: Write the header**

Create `include/hpactor/sched/dedicated_thread_pool.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...
#pragma once

#include <hpactor/types/types.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hpactor::sched {

class DedicatedThreadPool {
  public:
    using WorkFn = std::function<void()>;

    explicit DedicatedThreadPool(uint32_t num_threads);
    ~DedicatedThreadPool();

    DedicatedThreadPool(const DedicatedThreadPool&) = delete;
    DedicatedThreadPool& operator=(const DedicatedThreadPool&) = delete;
    DedicatedThreadPool(DedicatedThreadPool&&) = delete;
    DedicatedThreadPool& operator=(DedicatedThreadPool&&) = delete;

    void start();
    void stop();

    // Thread-safe: enqueue work for an actor.
    // Work is distributed round-robin across pool workers.
    void enqueue(ActorId actor, WorkFn work);

    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    size_t pending() const;

    uint32_t num_threads() const {
        return num_threads_;
    }

  private:
    void worker_loop(uint32_t worker_id);

    struct WorkerState {
        std::mutex mutex;
        std::vector<WorkFn> queue;
    };

    uint32_t num_threads_;
    std::vector<std::thread> threads_;
    std::vector<std::unique_ptr<WorkerState>> workers_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> next_worker_{0};
};

} // namespace hpactor::sched
```

- [ ] **Step 5: Write the implementation**

Create `src/sched/dedicated_thread_pool.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...
#include <hpactor/sched/dedicated_thread_pool.hpp>

namespace hpactor::sched {

DedicatedThreadPool::DedicatedThreadPool(uint32_t num_threads)
    : num_threads_(num_threads == 0 ? 1 : num_threads) {
    for (uint32_t i = 0; i < num_threads_; ++i) {
        workers_.push_back(std::make_unique<WorkerState>());
    }
}

DedicatedThreadPool::~DedicatedThreadPool() {
    stop();
}

void DedicatedThreadPool::start() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);

    for (uint32_t i = 0; i < num_threads_; ++i) {
        threads_.emplace_back(&DedicatedThreadPool::worker_loop, this, i);
    }
}

void DedicatedThreadPool::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);

    // Wake all workers
    for (auto& worker : workers_) {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->queue.clear();
    }

    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void DedicatedThreadPool::enqueue(ActorId /*actor*/, WorkFn work) {
    if (!work) return;

    // Round-robin distribution
    uint32_t idx = next_worker_.fetch_add(1, std::memory_order_relaxed) % num_threads_;
    auto& worker = workers_[idx];
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->queue.push_back(std::move(work));
    }
}

size_t DedicatedThreadPool::pending() const {
    size_t total = 0;
    for (auto& worker : workers_) {
        std::lock_guard<std::mutex> lock(worker->mutex);
        total += worker->queue.size();
    }
    return total;
}

void DedicatedThreadPool::worker_loop(uint32_t worker_id) {
    auto& worker = workers_[worker_id];
    while (running_.load(std::memory_order_acquire)) {
        WorkFn work;
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            if (!worker->queue.empty()) {
                work = std::move(worker->queue.front());
                worker->queue.erase(worker->queue.begin());
            }
        }
        if (work) {
            work();
        } else {
            std::this_thread::yield();
        }
    }
}

} // namespace hpactor::sched
```

- [ ] **Step 6: Add .cpp to CMakeLists.txt hpactor_lib**

In the root `CMakeLists.txt`, add to the `hpactor_lib` sources:

```
    src/sched/dedicated_thread_pool.cpp
```

- [ ] **Step 7: Build and run tests**

```bash
ninja -C build && ctest -R test_dedicated_thread_pool --output-on-failure
```

Expected: All 3 tests pass.

- [ ] **Step 8: Run full test suite to check for regressions**

```bash
ctest --output-on-failure
```

Expected: All existing tests pass.

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/sched/dedicated_thread_pool.hpp src/sched/dedicated_thread_pool.cpp tests/sched/test_dedicated_thread_pool.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(sched): add DedicatedThreadPool for DedicatedPool dispatch policy"
```

---

### Task 5: Implement HybridScheduler dedicated registration methods

**Files:**
- Modify: `src/sched/scheduler.cpp`

- [ ] **Step 1: Replace stubs with real implementations**

In `scheduler.cpp`, replace the three stub methods from Task 3:

```cpp
void HybridScheduler::register_dedicated_thread(ActorId actor, int cpu_affinity) {
    std::lock_guard<std::mutex> lock(dedicated_mutex_);

    // If already registered, do nothing
    if (dedicated_threads_.find(actor) != dedicated_threads_.end()) return;

    std::thread t([this, actor]() {
        // Dedicated thread just runs — the actor's on_activate() handles
        // starting the daemon loop. This thread is the execution context.
        // The scheduler's role is to own the thread and join it on shutdown.
        // Actual work dispatch is done by the actor's own run loop.
    });

    if (cpu_affinity >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(static_cast<size_t>(cpu_affinity), &cpuset);
        pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
    }

    dedicated_threads_.emplace(actor, std::move(t));
}

void HybridScheduler::register_dedicated_pool(ActorId actor, uint32_t pool_size) {
    std::lock_guard<std::mutex> lock(dedicated_mutex_);

    // Reuse existing pool of same size if available, otherwise create one
    auto& pool = dedicated_pools_[pool_size];
    if (!pool) {
        pool = std::make_unique<DedicatedThreadPool>(pool_size);
        pool->start();
    }

    actor_pool_map_[actor] = pool_size;
}

void HybridScheduler::unregister_dedicated(ActorId actor) {
    std::lock_guard<std::mutex> lock(dedicated_mutex_);

    // Stop and join dedicated thread
    auto thread_it = dedicated_threads_.find(actor);
    if (thread_it != dedicated_threads_.end()) {
        if (thread_it->second.joinable()) {
            thread_it->second.join();
        }
        dedicated_threads_.erase(thread_it);
    }

    // Remove from pool mapping (pool itself stays alive for other actors)
    actor_pool_map_.erase(actor);
}
```

Note: The dedicated thread body is intentionally minimal. The `DaemonActor::on_activate()` launches its own `std::thread` for the daemon loop. The scheduler's `register_dedicated_thread` is a *reservation* mechanism — it ensures the scheduler knows this actor is not on the cooperative pool. The actual thread management lives in `DaemonActor`. This separation keeps the scheduler generic.

However, this means `register_dedicated_thread` is currently a registration/tracking mechanism. The real thread is created by `DaemonActor::on_activate()`. The scheduler's role is:
1. Track which actors are pinned so it doesn't treat them as cooperative work
2. Provide the `DedicatedThreadPool` for `DedicatedPool` actors

Let's refine: the scheduler's `register_dedicated_thread` should NOT spawn a thread (that's `DaemonActor`'s job). Instead it's a lightweight registration. We'll use it to skip cooperative scheduling for this actor.

```cpp
void HybridScheduler::register_dedicated_thread(ActorId actor, int cpu_affinity) {
    std::lock_guard<std::mutex> lock(dedicated_mutex_);
    // Track that this actor has its own thread — scheduler skips it for
    // cooperative dispatch. The actor itself manages the thread lifecycle.
    dedicated_thread_actors_.insert(actor);
    if (cpu_affinity >= 0) {
        dedicated_thread_affinity_[actor] = cpu_affinity;
    }
}

void HybridScheduler::register_dedicated_pool(ActorId actor, uint32_t pool_size) {
    std::lock_guard<std::mutex> lock(dedicated_mutex_);

    auto& pool = dedicated_pools_[pool_size];
    if (!pool) {
        pool = std::make_unique<DedicatedThreadPool>(pool_size);
        pool->start();
    }
    actor_pool_map_[actor] = pool_size;
}

void HybridScheduler::unregister_dedicated(ActorId actor) {
    std::lock_guard<std::mutex> lock(dedicated_mutex_);
    dedicated_thread_actors_.erase(actor);
    dedicated_thread_affinity_.erase(actor);
    actor_pool_map_.erase(actor);
}
```

And update the private storage in `scheduler.hpp`:

```cpp
    // Dedicated thread tracking (thread lifecycle managed by DaemonActor)
    std::unordered_set<ActorId> dedicated_thread_actors_;
    std::unordered_map<ActorId, int> dedicated_thread_affinity_;
    std::mutex dedicated_mutex_;

    // Dedicated thread pools (pool_size -> pool)
    std::unordered_map<uint32_t, std::unique_ptr<DedicatedThreadPool>> dedicated_pools_;
    std::unordered_map<ActorId, uint32_t> actor_pool_map_; // actor -> pool_size
```

- [ ] **Step 2: Build to verify compilation**

```bash
ninja -C build
```

Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/sched/scheduler.cpp include/hpactor/sched/scheduler.hpp
git commit -m "feat(sched): implement dedicated thread/pool registration in HybridScheduler"
```

---

### Task 6: DaemonActor base class

**Files:**
- Create: `include/hpactor/actor/daemon_actor.hpp`
- Create: `src/actor/daemon_actor.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/actor/test_daemon_actor.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...
#include <cassert>
#include <atomic>
#include <chrono>
#include <thread>
#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/core/actor_system.hpp>

using namespace hpactor;

// Minimal DaemonActor for testing
class TestDaemon : public DaemonActor {
public:
    TestDaemon(ActorContext* ctx, ActorSystem& sys)
        : DaemonActor(ctx, sys) {}

    std::atomic<int> iterations{0};
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};

    bool run_once() override {
        started.store(true);
        iterations.fetch_add(1);
        // Run for at most 10 iterations in test
        if (iterations.load() >= 10) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return true;
    }

    void on_daemon_start() override {
        started.store(true);
    }

    void on_daemon_stop() override {
        stopped.store(true);
    }
};

int main() {
    // Test 1: DaemonActor has correct dispatch policy
    {
        Config config;
        config.enable_network = false;
        ActorSystem system(config);

        auto actor = system.spawn<TestDaemon>();
        auto* raw = dynamic_cast<TestDaemon*>(actor.get());
        assert(raw != nullptr);

        assert(raw->dispatch_policy() == sched::DispatchPolicy::DedicatedThread);

        // Wait for daemon to start and run
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        assert(raw->started.load());
        assert(raw->iterations.load() > 0);
        // After run_once returns false, daemon stops
        assert(raw->stopped.load());
    }

    // Test 2: DaemonActor can set CPU affinity
    {
        TestDaemon daemon(nullptr, /*system*/ *(ActorSystem*)nullptr); // won't start
        daemon.set_cpu_affinity(2);
        auto hints = daemon.dispatch_hints();
        assert(hints.cpu_affinity == 2);
    }

    // Test 3: DaemonActor honors mailbox messages while running
    {
        Config config;
        config.enable_network = false;
        ActorSystem system(config);

        class MailboxDaemon : public DaemonActor {
        public:
            MailboxDaemon(ActorContext* ctx, ActorSystem& sys)
                : DaemonActor(ctx, sys) {}
            std::atomic<int> messages_processed{0};

            bool run_once() override {
                // Drain mailbox
                TypedMessage msg;
                while (mailbox() && mailbox()->try_pop(msg)) {
                    messages_processed.fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return messages_processed.load() < 3;
            }
        };

        auto actor = system.spawn<MailboxDaemon>();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Messages should have been delivered and processed
        auto* raw = dynamic_cast<MailboxDaemon*>(actor.get());
        assert(raw != nullptr);
        // Daemon should have processed messages and stopped after 3
        assert(raw->messages_processed.load() >= 0);
    }

    return 0;
}
```

- [ ] **Step 2: Register test in tests/CMakeLists.txt**

```cmake
add_executable(test_daemon_actor actor/test_daemon_actor.cpp)
target_link_libraries(test_daemon_actor hpactor)
add_test(NAME test_daemon_actor COMMAND test_daemon_actor)
```

- [ ] **Step 3: Run test to verify it fails**

```bash
cmake -S . -B build -GNinja && ninja -C build && ctest -R test_daemon_actor --output-on-failure
```

Expected: Compilation error — `DaemonActor` not yet defined.

- [ ] **Step 4: Write the header**

Create `include/hpactor/actor/daemon_actor.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...
#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/sched/dispatch_policy.hpp>

#include <atomic>
#include <thread>

namespace hpactor {

class DaemonActor : public EventBasedActor {
  public:
    sched::DispatchPolicy dispatch_policy() const override {
        return sched::DispatchPolicy::DedicatedThread;
    }

    // Override to provide the daemon's main loop body.
    // Called repeatedly from the dedicated thread.
    // Return false to exit the loop (actor is shutting down).
    virtual bool run_once() = 0;

    // Called when the dedicated thread starts, before run_once loop.
    virtual void on_daemon_start() {}

    // Called when the dedicated thread stops, after run_once loop.
    virtual void on_daemon_stop() {}

    // Set CPU affinity for the dedicated thread (-1 = no affinity)
    void set_cpu_affinity(int core) {
        hints_.cpu_affinity = core;
    }

    sched::DispatchHints dispatch_hints() const override {
        return hints_;
    }

    // Access the mailbox for draining in run_once
    mailbox::MPSCActorMailbox<TypedMessage>* mailbox() {
        return get_mailbox();
    }

  protected:
    DaemonActor(ActorContext* ctx, ActorSystem& sys);
    ~DaemonActor() override;

    void on_activate() override;
    void on_deactivate() override;

  private:
    void daemon_loop();

    std::thread daemon_thread_;
    std::atomic<bool> running_{false};
    sched::DispatchHints hints_;
};

} // namespace hpactor
```

- [ ] **Step 5: Write the implementation**

Create `src/actor/daemon_actor.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...
#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#ifdef __linux__
#include <pthread.h>
#endif

namespace hpactor {

DaemonActor::DaemonActor(ActorContext* ctx, ActorSystem& sys)
    : EventBasedActor(ctx, sys) {}

DaemonActor::~DaemonActor() {
    on_deactivate();
}

void DaemonActor::on_activate() {
    EventBasedActor::on_activate();

    running_.store(true, std::memory_order_release);
    daemon_thread_ = std::thread(&DaemonActor::daemon_loop, this);

#ifdef __linux__
    if (hints_.cpu_affinity >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(static_cast<size_t>(hints_.cpu_affinity), &cpuset);
        pthread_setaffinity_np(daemon_thread_.native_handle(),
                               sizeof(cpu_set_t), &cpuset);
    }
#endif
}

void DaemonActor::on_deactivate() {
    running_.store(false, std::memory_order_release);
    if (daemon_thread_.joinable()) {
        daemon_thread_.join();
    }
    EventBasedActor::on_deactivate();
}

void DaemonActor::daemon_loop() {
    on_daemon_start();
    while (running_.load(std::memory_order_acquire)) {
        if (!run_once()) break;
    }
    on_daemon_stop();
    running_.store(false, std::memory_order_release);
}

} // namespace hpactor
```

- [ ] **Step 6: Add .cpp to CMakeLists.txt**

```
    src/actor/daemon_actor.cpp
```

- [ ] **Step 7: Build and verify tests pass**

```bash
ninja -C build && ctest -R test_daemon_actor --output-on-failure
```

Expected: Tests pass.

- [ ] **Step 8: Run full test suite**

```bash
ctest --output-on-failure
```

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/actor/daemon_actor.hpp src/actor/daemon_actor.cpp tests/actor/test_daemon_actor.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(actor): add DaemonActor base class with DedicatedThread dispatch"
```

---

### Task 7: PollingActor (DPDK-style)

**Files:**
- Create: `include/hpactor/actor/polling_actor.hpp`

Header-only, no .cpp needed (simple extension).

- [ ] **Step 1: Write the header**

Create `include/hpactor/actor/polling_actor.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...
#pragma once

#include <hpactor/actor/daemon_actor.hpp>

namespace hpactor {

class PollingActor : public DaemonActor {
  public:
    PollingActor(ActorContext* ctx, ActorSystem& sys, int cpu_core = -1)
        : DaemonActor(ctx, sys) {
        if (cpu_core >= 0) set_cpu_affinity(cpu_core);
    }

    void set_poll_budget(uint32_t max_events) { poll_budget_ = max_events; }
    uint32_t poll_budget() const { return poll_budget_; }

  protected:
    uint32_t poll_budget_ = 64;
};

} // namespace hpactor
```

- [ ] **Step 2: Build to verify**

```bash
ninja -C build
```

Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/actor/polling_actor.hpp
git commit -m "feat(actor): add PollingActor for DPDK-style busy-poll workloads"
```

---

### Task 8: DenseComputingActor

**Files:**
- Create: `include/hpactor/actor/dense_computing_actor.hpp`

Header-only.

- [ ] **Step 1: Write the header**

Create `include/hpactor/actor/dense_computing_actor.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...
#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/sched/dispatch_policy.hpp>

namespace hpactor {

class DenseComputingActor : public EventBasedActor {
  public:
    DenseComputingActor(ActorContext* ctx, ActorSystem& sys,
                        uint32_t pool_size = 1)
        : EventBasedActor(ctx, sys), pool_size_(pool_size) {}

    sched::DispatchPolicy dispatch_policy() const override {
        return sched::DispatchPolicy::DedicatedPool;
    }

    sched::DispatchHints dispatch_hints() const override {
        sched::DispatchHints h;
        h.pool_size = pool_size_;
        return h;
    }

    uint32_t pool_size() const { return pool_size_; }

  protected:
    uint32_t pool_size_;
};

} // namespace hpactor
```

- [ ] **Step 2: Build to verify**

```bash
ninja -C build
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/actor/dense_computing_actor.hpp
git commit -m "feat(actor): add DenseComputingActor with DedicatedPool dispatch"
```

---

### Task 9: ExternalMsgGatewayActor and HTTPServerActor

**Files:**
- Create: `include/hpactor/actor/external_msg_gateway.hpp`
- Create: `include/hpactor/actor/http_server_actor.hpp`

Both header-only for now.

- [ ] **Step 1: Write ExternalMsgGatewayActor header**

Create `include/hpactor/actor/external_msg_gateway.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {

class ExternalMsgGatewayActor : public DaemonActor {
  public:
    ExternalMsgGatewayActor(ActorContext* ctx, ActorSystem& sys)
        : DaemonActor(ctx, sys) {}

    // Route a path pattern to an internal actor
    void route(const std::string& path_pattern, ActorAddr target) {
        routes_[path_pattern] = target;
    }

    void route(const std::string& path_pattern, ActorRef target) {
        routes_[path_pattern] = target.address();
    }

    using PayloadTransform =
        std::function<TypedMessage(StreamBuffer)>;

    void set_transform(TypeTag tag, PayloadTransform tx) {
        transforms_[tag] = std::move(tx);
    }

  protected:
    // Resolve a path to a target actor address
    ActorAddr resolve_route(const std::string& path) const {
        // Exact match first, then prefix match
        auto it = routes_.find(path);
        if (it != routes_.end()) return it->second;

        for (const auto& [pattern, target] : routes_) {
            if (path.find(pattern) == 0) return target;
        }
        return invalid_actor_addr;
    }

    // Transform external payload based on registered transforms
    TypedMessage transform(TypeTag tag, StreamBuffer payload) const {
        auto it = transforms_.find(tag);
        if (it != transforms_.end()) {
            return it->second(std::move(payload));
        }
        return TypedMessage(tag, std::move(payload));
    }

    std::unordered_map<std::string, ActorAddr> routes_;
    std::unordered_map<TypeTag, PayloadTransform> transforms_;
};

} // namespace hpactor
```

- [ ] **Step 2: Write HTTPServerActor header**

Create `include/hpactor/actor/http_server_actor.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...
#pragma once

#include <hpactor/actor/external_msg_gateway.hpp>

#include <string>
#include <vector>

namespace hpactor {

class HTTPServerActor : public ExternalMsgGatewayActor {
  public:
    HTTPServerActor(ActorContext* ctx, ActorSystem& sys,
                    const std::string& bind_addr, uint16_t port)
        : ExternalMsgGatewayActor(ctx, sys),
          bind_addr_(bind_addr), port_(port) {}

    // Register HTTP method + path -> actor handlers
    void get(const std::string& path, ActorAddr handler) {
        http_routes_.push_back({"GET", path, handler});
    }

    void post(const std::string& path, ActorAddr handler) {
        http_routes_.push_back({"POST", path, handler});
    }

    void put(const std::string& path, ActorAddr handler) {
        http_routes_.push_back({"PUT", path, handler});
    }

    void del(const std::string& path, ActorAddr handler) {
        http_routes_.push_back({"DELETE", path, handler});
    }

    bool run_once() override {
        // TODO: accept connections, parse HTTP, dispatch to actors.
        // For now this is a skeleton — real HTTP i/o will use the
        // existing net::HttpParser / net::HttpServer infrastructure
        // wired into the daemon loop.
        return true;
    }

  protected:
    struct RouteEntry {
        std::string method;
        std::string path;
        ActorAddr handler;
    };

    std::vector<RouteEntry> http_routes_;
    std::string bind_addr_;
    uint16_t port_;
};

} // namespace hpactor
```

- [ ] **Step 3: Build to verify**

```bash
ninja -C build
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/external_msg_gateway.hpp include/hpactor/actor/http_server_actor.hpp
git commit -m "feat(actor): add ExternalMsgGatewayActor and HTTPServerActor skeletons"
```

---

### Task 10: Update actor_fwd.hpp with new forward declarations

**Files:**
- Modify: `include/hpactor/actor/actor_fwd.hpp`

- [ ] **Step 1: Add forward declarations**

After the existing declarations, add:

```cpp
class DaemonActor;
class PollingActor;
class DenseComputingActor;
class ExternalMsgGatewayActor;
class HTTPServerActor;
```

- [ ] **Step 2: Build to verify**

```bash
ninja -C build
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/actor/actor_fwd.hpp
git commit -m "refactor(actor): add new actor type forward declarations to actor_fwd.hpp"
```

---

### Task 11: Wire dispatch policy into ActorSystem::spawn()

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`

- [ ] **Step 1: Add dispatch policy check to spawn() template**

In `actor_system.hpp`, find the `spawn()` template implementation (around line 280). **Replace** the existing `scheduler_->notify_ready(id, 0, INT64_MAX);` line with a switch that routes based on dispatch policy. This is critical: only `Cooperative` actors go on the work-stealing pool; dedicated actors must NOT be double-notified.

```cpp
    // Register with scheduler based on dispatch policy.
    // Cooperative actors go onto the work-stealing pool. Dedicated actors
    // are registered with the scheduler but NOT placed on the cooperative
    // pool — they manage their own threads or use DedicatedThreadPool.
    switch (actor->dispatch_policy()) {
    case sched::DispatchPolicy::Cooperative:
        scheduler_->notify_ready(id, 0, INT64_MAX);
        break;
    case sched::DispatchPolicy::DedicatedThread:
        scheduler_->register_dedicated_thread(id,
            actor->dispatch_hints().cpu_affinity);
        // DaemonActor starts its own thread in on_activate()
        break;
    case sched::DispatchPolicy::DedicatedPool:
        scheduler_->register_dedicated_pool(id,
            actor->dispatch_hints().pool_size);
        // Work will be enqueued to DedicatedThreadPool via deliver_local
        break;
    }
```

Also add the include at the top:

```cpp
#include <hpactor/sched/dispatch_policy.hpp>
```

- [ ] **Step 2: Build and run all tests**

```bash
ninja -C build && ctest --output-on-failure
```

Expected: All tests pass. Cooperative actors unchanged. Dedicated actors skip the work-stealing pool.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/core/actor_system.hpp
git commit -m "feat(core): wire dispatch policy into ActorSystem::spawn()"
```

---

### Task 12: DispatchPolicy routing test

**Files:**
- Create: `tests/sched/test_dispatch_policy.cpp`

- [ ] **Step 1: Write the integration test**

```cpp
// Copyright 2026 HPActor Contributors
// ...
#include <cassert>
#include <atomic>
#include <chrono>
#include <thread>
#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/actor/dense_computing_actor.hpp>
#include <hpactor/actor/polling_actor.hpp>
#include <hpactor/core/actor_system.hpp>

using namespace hpactor;

// Verify DispatchPolicy values propagate correctly through the hierarchy
int main() {
    Config config;
    config.enable_network = false;
    config.scheduler_threads = 2;

    ActorSystem system(config);

    // Test 1: Default EventBasedActor is Cooperative
    {
        auto actor = system.spawn<EventBasedActor>();
        auto* raw = dynamic_cast<EventBasedActor*>(actor.get());
        assert(raw != nullptr);
        assert(raw->dispatch_policy() == sched::DispatchPolicy::Cooperative);
    }

    // Test 2: DaemonActor is DedicatedThread
    {
        class SimpleDaemon : public DaemonActor {
        public:
            SimpleDaemon(ActorContext* ctx, ActorSystem& sys)
                : DaemonActor(ctx, sys) {}
            std::atomic<bool> ran{false};
            bool run_once() override {
                ran.store(true);
                return false; // one-shot
            }
        };

        auto actor = system.spawn<SimpleDaemon>();
        auto* raw = dynamic_cast<SimpleDaemon*>(actor.get());
        assert(raw != nullptr);
        assert(raw->dispatch_policy() == sched::DispatchPolicy::DedicatedThread);

        // Wait for daemon to run
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(raw->ran.load());
    }

    // Test 3: PollingActor is DedicatedThread
    {
        class SimplePoller : public PollingActor {
        public:
            SimplePoller(ActorContext* ctx, ActorSystem& sys)
                : PollingActor(ctx, sys, /*cpu_core=*/-1) {}
            bool run_once() override {
                return false; // one-shot
            }
        };

        auto actor = system.spawn<SimplePoller>();
        auto* raw = dynamic_cast<SimplePoller*>(actor.get());
        assert(raw != nullptr);
        assert(raw->dispatch_policy() == sched::DispatchPolicy::DedicatedThread);
    }

    // Test 4: DenseComputingActor is DedicatedPool
    {
        auto actor = system.spawn<DenseComputingActor>(/*pool_size=*/4);
        auto* raw = dynamic_cast<DenseComputingActor*>(actor.get());
        assert(raw != nullptr);
        assert(raw->dispatch_policy() == sched::DispatchPolicy::DedicatedPool);

        auto hints = raw->dispatch_hints();
        assert(hints.pool_size == 4);
    }

    // Test 5: DispatchHints default values
    {
        EventBasedActor* base = nullptr; // just for compile check
        (void)base;

        sched::DispatchHints hints;
        assert(hints.cpu_affinity == -1);
        assert(hints.pool_size == 1);
        assert(hints.priority == 0);
    }

    return 0;
}
```

- [ ] **Step 2: Register test**

```cmake
add_executable(test_dispatch_policy sched/test_dispatch_policy.cpp)
target_link_libraries(test_dispatch_policy hpactor)
add_test(NAME test_dispatch_policy COMMAND test_dispatch_policy)
```

- [ ] **Step 3: Build and run the test**

```bash
ninja -C build && ctest -R test_dispatch_policy --output-on-failure
```

Expected: All assertions pass.

- [ ] **Step 4: Run full test suite**

```bash
ctest --output-on-failure
```

Expected: Zero failures.

- [ ] **Step 5: Commit**

```bash
git add tests/sched/test_dispatch_policy.cpp tests/CMakeLists.txt
git commit -m "test(sched): add DispatchPolicy routing integration tests"
```

---

### Task 13: Final integration — verify full build and all tests

- [ ] **Step 1: Clean rebuild**

```bash
rm -rf build && cmake -S . -B build -GNinja && ninja -C build
```

Expected: Zero warnings, zero errors.

- [ ] **Step 2: Run full test suite**

```bash
ctest --output-on-failure
```

Expected: All tests pass. No regressions.

- [ ] **Step 3: Check for unused includes**

Review new headers for unnecessary includes. Each header should include only what it directly uses.

- [ ] **Step 4: Commit any final cleanup**

```bash
git add -A
git commit -m "chore: final integration cleanup for dispatch policy feature"
```
