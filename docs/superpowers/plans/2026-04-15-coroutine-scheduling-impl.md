# Coroutine Scheduling Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement the coroutine scheduling subsystem from `2026-04-15-coroutine-scheduling-design.md`.

**Current State (from review):**
- `ChaselevDeque`, `MultiPriorityWorkQueue`, `EDFQueue`, `A2WS`, `TimingWheel`, `CoroutineFramePool` — **COMPLETE**
- `IScheduler` interface — **WRONG** (method names/signatures don't match spec)
- Worker threads — **NEVER STARTED** (`start()` doesn't spawn threads)
- Work-stealing in `WorkerThread` — **STUB** (always returns false)
- `CoroutineTask`, `CoroutinePromise`, `MailboxAwaiter`, `TimerAwaiter` — **DO NOT EXIST**
- `ActorState` state machine — **NOT IMPLEMENTED**
- `EventBasedActor` — behavior-based, **not coroutine-based**

**Reference:** `docs/superpowers/specs/2026-04-15-coroutine-scheduling-design.md`

---

## Phase 0: Fix IScheduler Interface Mismatch

The existing `IScheduler` has `enqueue()`/`enqueue_deadline()`, but the spec defines `notify_ready()`/`notify_idle()`/`schedule_after()`/`schedule_every()`/`cancel_timer()`. These are fundamentally different APIs.

### Task 0.1: Update `IScheduler` interface in `include/hpactor/sched/scheduler.hpp`

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp`
- Depends on: None

**Tasks:**
- [ ] **Step 1: Read existing `scheduler.hpp` header**

```bash
cat include/hpactor/sched/scheduler.hpp
```

- [ ] **Step 2: Replace the IScheduler interface**

The interface currently has:
```cpp
virtual void enqueue(ActorId actor, uint8_t priority) = 0;
virtual void enqueue_deadline(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;
virtual bool is_running() const = 0;
virtual uint32_t num_workers() const = 0;
```

Replace with spec-compliant interface:
```cpp
// Thread-safe; may be called from any thread including I/O threads
virtual void notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;
virtual void notify_idle(ActorId actor) = 0;
virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;
virtual TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) = 0;
virtual void cancel_timer(TimerHandle handle) = 0;
virtual size_t worker_count() const = 0;
```

- [ ] **Step 3: Add `TimerHandle` and `timer_callback` to header**

```cpp
struct TimerHandle {
    uint64_t id = 0;
    bool valid() const noexcept { return id != 0; }
};

using timer_callback = std::function<void()>;
```

- [ ] **Step 4: Verify compilation**

```bash
ninja -C build 2>&1 | head -30
# Expected: errors — HybridScheduler methods no longer match interface
```

---

### Task 0.2: Update `HybridScheduler` implementation in `src/sched/scheduler.cpp`

**Files:**
- Modify: `src/sched/scheduler.cpp`
- Depends on: Task 0.1

**Tasks:**
- [ ] **Step 1: Read existing `scheduler.cpp`**

```bash
cat src/sched/scheduler.cpp
```

- [ ] **Step 2: Rename `enqueue()` → `notify_ready()`, `enqueue_deadline()` → `notify_ready()` with deadline**

Current code:
```cpp
void HybridScheduler::enqueue(ActorId actor, uint8_t priority) { ... }
void HybridScheduler::enqueue_deadline(ActorId actor, uint8_t priority, int64_t deadline_ns) { ... }
```

Replace with:
```cpp
void HybridScheduler::notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    // Select target worker using A2WS
    size_t worker_idx = select_worker(priority);

    // Upsert to EDF queue for deadline tracking
    edf_queue_.upsert({actor, priority, deadline_ns, worker_idx});

    // Push to worker's deque
    WorkItem item{actor, deadline_ns, next_sequence_.fetch_add(1, std::memory_order_relaxed)};
    workers_[worker_idx].queues[priority].push_bottom(item);
}

void HybridScheduler::notify_idle(ActorId actor) {
    edf_queue_.remove(actor);
}
```

- [ ] **Step 3: Add stub `schedule_after()`, `schedule_every()`, `cancel_timer()`**

```cpp
TimerHandle HybridScheduler::schedule_after(timer_callback /*cb*/, int64_t /*delay_ns*/) {
    // Phase 4: integrate with TimingWheel
    return TimerHandle{0};
}

TimerHandle HybridScheduler::schedule_every(timer_callback /*cb*/, int64_t /*interval_ns*/) {
    return TimerHandle{0};
}

void HybridScheduler::cancel_timer(TimerHandle /*handle*/) {
    // Phase 4
}
```

- [ ] **Step 4: Verify compilation**

```bash
ninja -C build 2>&1 | head -30
```

---

### Task 0.3: Update `ActorSystem` to use new scheduler interface

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`, `src/actor/actor_system.cpp`
- Depends on: Task 0.2

**Tasks:**
- [ ] **Step 1: Find where `scheduler_->enqueue()` is called**

```bash
grep -rn "scheduler_->enqueue\|scheduler_->enqueue_deadline" src/
```

- [ ] **Step 2: Replace with `scheduler_->notify_ready()`**

Wherever `scheduler_->enqueue(target, 0)` is called, replace with:
```cpp
scheduler_->notify_ready(target, 0, INT64_MAX);
```

Wherever `scheduler_->enqueue_deadline(target, priority, deadline)` is called, replace with:
```cpp
scheduler_->notify_ready(target, priority, deadline);
```

- [ ] **Step 3: Verify compilation**

```bash
ninja -C build 2>&1 | head -30
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp
git commit -m "fix(sched): align IScheduler interface with design spec

Rename enqueue()/enqueue_deadline() to notify_ready() with unified signature.
Add notify_idle(), schedule_after(), schedule_every(), cancel_timer() stubs.
Update ActorSystem call sites.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 1: Start Worker Threads + Implement Work-Stealing

### Task 1.1: Fix `HybridScheduler::start()` to spawn worker threads

**Files:**
- Modify: `src/sched/scheduler.cpp`
- Depends on: Phase 0

**Tasks:**
- [ ] **Step 1: Read current `start()` implementation**

Current code:
```cpp
void HybridScheduler::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(true, std::memory_order_release);
}
```

- [ ] **Step 2: Update to spawn worker threads**

```cpp
void HybridScheduler::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(true, std::memory_order_release);

    for (size_t i = 0; i < workers_.size(); ++i) {
        workers_[i].start();
    }
}
```

> **Note:** `workers_[i]` are `WorkerState` structs (not `WorkerThread` objects). The current code uses `std::vector<WorkerState>` where each `WorkerState` has `queues` (array of `ChaselevDeque`) and `edf_queue`. The `WorkerThread` class in `worker_thread.hpp` is separate and not yet wired in.

**Check the actual worker type:**

```bash
grep -n "workers_" src/sched/scheduler.cpp
grep -n "WorkerState\|WorkerThread" include/hpactor/sched/scheduler.hpp
```

The `HybridScheduler` currently uses `struct WorkerState` with `std::unique_ptr<ChaselevDeque<WorkItem>[]> queues` and `EDFQueue edf_queue`. The `WorkerThread` class exists but is separate.

**Decide:** Either (a) use the existing `WorkerThread` class properly, or (b) keep the inline `WorkerState` but add a thread. For simplicity, add a `std::thread` per worker directly to `HybridScheduler`.

- [ ] **Step 3: Add worker threads to `HybridScheduler`**

```cpp
// In scheduler.hpp, add to HybridScheduler:
std::vector<std::thread> worker_threads_;
```

- [ ] **Step 4: Update `start()` to spawn threads**

```cpp
void HybridScheduler::start() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);

    worker_threads_.reserve(workers_.size());
    for (size_t i = 0; i < workers_.size(); ++i) {
        worker_threads_.emplace_back([this, i] { worker_loop(i); });
    }
}

void HybridScheduler::stop() {
    running_.store(false, std::memory_order_release);
    for (auto& t : worker_threads_) {
        if (t.joinable()) t.join();
    }
    worker_threads_.clear();
}
```

- [ ] **Step 5: Implement `worker_loop()`**

```cpp
void HybridScheduler::worker_loop(size_t worker_id) {
    while (running_.load(std::memory_order_acquire)) {
        WorkItem item;

        // 1. Try local pop (owner - wait-free)
        if (pop_local(item, worker_id)) {
            process_actor(item);
            continue;
        }

        // 2. Try EDF
        if (pop_edf(item, worker_id)) {
            process_actor(item);
            continue;
        }

        // 3. Try stealing
        if (try_steal(item)) {
            process_actor(item);
            continue;
        }

        // 4. No work - yield
        std::this_thread::yield();
    }
}
```

- [ ] **Step 6: Verify compilation**

```bash
ninja -C build 2>&1 | head -40
```

---

### Task 1.2: Fix `WorkerThread::try_steal()` to actually steal

**Files:**
- Modify: `include/hpactor/sched/worker_thread.hpp`, `src/sched/worker_thread.cpp`
- Depends on: Task 1.1

**Tasks:**
- [ ] **Step 1: Read current `try_steal()` implementation**

Current stub at `worker_thread.cpp:51-59`:
```cpp
bool WorkerThread::steal(WorkItem& out) {
    for (uint32_t i = 0; i < local_queue_.num_levels(); ++i) {
        if (local_queue_.steal(out)) {
            return true;
        }
    }
    return false;
}
```

- [ ] **Step 2: Implement actual steal logic using A2WS**

Update `try_steal()` to use A2WS victim selection:

```cpp
bool WorkerThread::try_steal(WorkItem& out) {
    uint32_t my_id = config_.worker_index;

    // Try up to victim_scan_limit victims
    for (uint32_t attempt = 0; attempt < config_.victim_scan_limit; ++attempt) {
        uint32_t victim_idx = owner_.a2ws().get_victim(my_id);

        if (victim_idx == SIZE_MAX || victim_idx >= owner_.workers().size()) {
            break;
        }

        auto& victim = owner_.workers()[victim_idx];

        // Try EDF queue first
        if (victim.edf_queue.pop(out)) {
            owner_.a2ws().record_steal(my_id, victim_idx);
            return true;
        }

        // Try each priority level
        for (uint32_t p = 0; p < local_queue_.num_levels(); ++p) {
            if (victim.queues[p].steal_top(out)) {
                owner_.a2ws().record_steal(my_id, victim_idx);
                return true;
            }
        }

        owner_.a2ws().record_attempt(my_id, victim_idx, false);
    }
    return false;
}
```

- [ ] **Step 3: Expose `a2ws()` and `workers()` from `HybridScheduler`**

In `scheduler.hpp`:
```cpp
friend class WorkerThread;
A2WS& a2ws() { return a2ws_; }
std::vector<WorkerState>& workers() { return workers_; }
```

- [ ] **Step 4: Verify compilation**

```bash
ninja -C build 2>&1 | head -40
```

---

### Task 1.3: Commit Phase 1

```bash
git add include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp include/hpactor/sched/worker_thread.hpp src/sched/worker_thread.cpp
git commit -m "feat(sched): start worker threads and implement work-stealing

HybridScheduler::start() now spawns N worker threads.
worker_loop() implements: local pop → EDF pop → steal → yield.
WorkerThread::try_steal() uses A2WS victim selection and actually
steals from EDF queue and priority levels.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 2: Implement ActorState + CoroutineTask/CoroutinePromise

### Task 2.1: Create `include/hpactor/sched/actor_state.hpp`

**Files:**
- Create: `include/hpactor/sched/actor_state.hpp`
- Depends on: Phase 1

**Tasks:**
- [ ] **Step 1: Write header**

```cpp
// include/hpactor/sched/actor_state.hpp
#pragma once

#include <atomic>
#include <cstdint>

namespace hpactor::sched {

// ActorState: atomic state encoding for actor lifecycle
// States: Idle(0) → Ready(1) → Running(2) → IOWaiting(3) / Terminated(4)
class ActorState {
public:
    static constexpr uint32_t kIdle       = 0x01;
    static constexpr uint32_t kReady      = 0x02;
    static constexpr uint32_t kRunning    = 0x04;
    static constexpr uint32_t kIOWaiting  = 0x08;
    static constexpr uint32_t kTerminated = 0x10;
    static constexpr uint32_t kMask       = 0x1F;

    ActorState() : state_(kIdle) {}
    explicit ActorState(uint32_t initial) : state_(initial) {}

    uint32_t get() const {
        return state_.load(std::memory_order_acquire);
    }

    // CAS transition. Returns true if successful.
    bool cas(uint32_t expected, uint32_t desired) {
        return state_.compare_exchange_strong(expected, desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire);
    }

    void set(uint32_t s) {
        state_.store(s, std::memory_order_release);
    }

    bool is_idle() const { return get() == kIdle; }
    bool is_ready() const { return get() == kReady; }
    bool is_running() const { return get() == kRunning; }
    bool is_io_waiting() const { return get() == kIOWaiting; }
    bool is_terminated() const { return get() == kTerminated; }

private:
    std::atomic<uint32_t> state_;
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Verify compilation**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/sched/actor_state.hpp -o /dev/null
```

---

### Task 2.2: Create `include/hpactor/sched/coroutine_task.hpp`

**Files:**
- Create: `include/hpactor/sched/coroutine_task.hpp`
- Depends on: Task 2.1

**Tasks:**
- [ ] **Step 1: Write header**

```cpp
// include/hpactor/sched/coroutine_task.hpp
#pragma once

#include <hpactor/sched/actor_state.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <thread>

namespace hpactor::sched {

// Forward declarations
class ActorCoroutine;
class WorkerThread;

// CoroutineTask: return type of actor coroutines
// Wraps std::coroutine_handle<CoroutinePromise> and manages actor lifecycle
class CoroutineTask {
public:
    using handle_type = std::coroutine_handle<CoroutinePromise>;

    CoroutineTask() noexcept : handle_(nullptr) {}
    explicit CoroutineTask(handle_type handle) noexcept : handle_(handle) {}

    CoroutineTask(CoroutineTask&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    CoroutineTask(const CoroutineTask&) = delete;
    CoroutineTask& operator=(const CoroutineTask&) = delete;

    CoroutineTask& operator=(CoroutineTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~CoroutineTask() {
        if (handle_) handle_.destroy();
    }

    explicit operator bool() const noexcept { return handle_; }

    handle_type handle() const noexcept { return handle_; }

    // Resume the coroutine
    void resume() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
        }
    }

    // Check if done
    bool done() const { return !handle_ || handle_.done(); }

private:
    handle_type handle_;
};

// CoroutinePromise: promise_type for actor coroutines
// Controls lifecycle: initial_suspend → Running → (Suspend | Terminate)
struct CoroutinePromise {
    using handle_type = std::coroutine_handle<CoroutinePromise>;

    ActorId actor_id;
    ActorState state;
    WorkerThread* owner{nullptr};

    // Mailbox integration
    void* mailbox{nullptr};  // MPSCMailbox<MessageNode>*
    std::atomic<bool> mailbox_was_empty{true};

    // Continuation for chained awaiters
    std::coroutine_handle<> continuation;

    CoroutinePromise() = default;
    ~CoroutinePromise() = default;

    // Start suspended — scheduler decides when to resume
    std::suspend_always initial_suspend() noexcept { return {}; }

    // Called on co_return
    void return_void() noexcept {
        state.set(ActorState::kTerminated);
    }

    // Called on unhandled exception
    void unhandled_exception() noexcept {
        state.set(ActorState::kTerminated);
        // Store exception info for error reporting (future)
    }

    CoroutineTask get_return_object() {
        return CoroutineTask{handle_type::from_promise(*this)};
    }

    // State access
    void set_running() { state.set(ActorState::kRunning); }
    void set_idle() { state.set(ActorState::kIdle); }
    void set_ready() { state.set(ActorState::kReady); }
    void set_io_waiting() { state.set(ActorState::kIOWaiting); }
    void set_terminated() { state.set(ActorState::kTerminated); }

    bool is_idle() const { return state.is_idle(); }
    bool is_running() const { return state.is_running(); }
    bool is_terminated() const { return state.is_terminated(); }
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Verify compilation**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/sched/coroutine_task.hpp -o /dev/null 2>&1
```

---

### Task 2.3: Commit Phase 2 (so far)

```bash
git add include/hpactor/sched/actor_state.hpp include/hpactor/sched/coroutine_task.hpp
git commit -m "feat(sched): add ActorState and CoroutineTask/CoroutinePromise

ActorState: atomic state encoding (Idle/Ready/Running/IOWaiting/Terminated)
with CAS-based transitions.
CoroutineTask: coroutine handle wrapper with move semantics.
CoroutinePromise: promise_type for actor coroutines with lifecycle state.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 3: Implement MailboxAwaiter + TimerAwaiter

### Task 3.1: Create `include/hpactor/sched/coroutine_awaiters.hpp`

**Files:**
- Create: `include/hpactor/sched/coroutine_awaiters.hpp`
- Depends on: Phase 2

**Tasks:**
- [ ] **Step 1: Write MailboxAwaiter**

```cpp
// include/hpactor/sched/coroutine_awaiters.hpp
#pragma once

#include <hpactor/sched/coroutine_task.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>

namespace hpactor::sched {

// MailboxAwaiter: awaitable for co_await actor.receive()
// Suspends when mailbox is empty, resumes when message arrives
class MailboxAwaiter {
public:
    explicit MailboxAwaiter(CoroutinePromise& promise) noexcept
        : promise_(promise) {}

    // Return true if already has message (don't suspend)
    bool await_ready() const noexcept {
        return !promise_.mailbox_was_empty.load(std::memory_order_acquire);
    }

    // Called when suspending
    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        // Transition: Running → Idle
        uint32_t expected = ActorState::kRunning;
        if (promise_.state.cas(expected, ActorState::kIdle)) {
            promise_.continuation = continuation;
            return true;  // successfully suspended
        }
        // State was not Running — actor may have already terminated
        return false;  // don't suspend
    }

    // Called when resuming (message arrived)
    void await_resume() noexcept {
        // State should already be Ready or Running
    }

private:
    CoroutinePromise& promise_;
};

// TimerAwaiter: awaitable for co_await scheduler.schedule_after(delay)
class TimerAwaiter {
public:
    TimerAwaiter(int64_t delay_ns, uint64_t& timer_id_out,
                 std::coroutine_handle<>& cont_out) noexcept
        : delay_ns_(delay_ns), timer_id_out_(timer_id_out), continuation_(cont_out) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
        // Caller will schedule timer and set timer_id_out_
        return true;
    }

    void await_resume() noexcept {
        // Timer fired — continuation was already resumed
    }

private:
    int64_t delay_ns_;
    uint64_t& timer_id_out_;
    std::coroutine_handle<>& continuation_;
};

// BlockingMailboxAwaiter: for blocking receive with stackful coroutines
class BlockingMailboxAwaiter {
public:
    BlockingMailboxAwaiter(CoroutinePromise& promise,
                           void* frame_pool,
                           std::coroutine_handle<> continuation) noexcept
        : promise_(promise), frame_pool_(frame_pool), continuation_(continuation) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        promise_.continuation = continuation;
        return true;
    }

    void await_resume() noexcept {
        // Returns the message
    }

private:
    CoroutinePromise& promise_;
    void* frame_pool_;
    std::coroutine_handle<> continuation_;
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Verify compilation**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/sched/coroutine_awaiters.hpp -o /dev/null 2>&1
```

---

### Task 3.2: Commit Phase 3

```bash
git add include/hpactor/sched/coroutine_awaiters.hpp
git commit -m "feat(sched): add MailboxAwaiter, TimerAwaiter, BlockingMailboxAwaiter

MailboxAwaiter: suspends actor coroutine when mailbox empty, resumes on message.
TimerAwaiter: suspends until delay expires.
BlockingMailboxAwaiter: for stackful coroutine blocking receive.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 4: Wire Coroutines into HybridScheduler

### Task 4.1: Add `execute_actor()` with coroutine resumption

**Files:**
- Modify: `src/sched/scheduler.cpp`
- Depends on: Phase 3

**Tasks:**
- [ ] **Step 1: Add `execute_actor()` method**

```cpp
void HybridScheduler::execute_actor(const WorkItem& item) {
    auto actor_ptr = system_.get_actor(item.actor);
    if (!actor_ptr) {
        return;
    }

    auto* coroutine = actor_ptr->get_coroutine();
    if (!coroutine) {
        // Fall back to non-coroutine receive path
        process_actor(item);
        return;
    }

    // State: Ready → Running
    uint32_t expected = ActorState::kReady;
    if (!coroutine->state.cas(expected, ActorState::kRunning)) {
        // Actor not in Ready state — may have been rescheduled or terminated
        return;
    }

    // Resume the coroutine
    coroutine->handle().resume();

    // When we return here:
    // - Coroutine suspended (co_await) → state is Idle or IOWaiting
    // - Coroutine finished → state is Terminated

    if (coroutine->is_idle() && actor_ptr->mailbox_has_messages()) {
        // Mailbox has more work — re-schedule
        notify_ready(item.actor, 0, INT64_MAX);
    }
}
```

- [ ] **Step 2: Update `worker_loop()` to call `execute_actor()` with coroutine**

```cpp
void HybridScheduler::worker_loop(size_t worker_id) {
    while (running_.load(std::memory_order_acquire)) {
        WorkItem item;

        if (pop_local(item, worker_id)) {
            execute_actor(item);
            continue;
        }

        if (pop_edf(item, worker_id)) {
            execute_actor(item);
            continue;
        }

        if (try_steal(item)) {
            execute_actor(item);
            continue;
        }

        std::this_thread::yield();
    }
}
```

- [ ] **Step 3: Verify compilation**

```bash
ninja -C build 2>&1 | head -50
```

---

### Task 4.2: Add `get_coroutine()` and `mailbox_has_messages()` to actor interface

**Files:**
- Modify: `include/hpactor/actor/event_based_actor.hpp`
- Depends on: Task 4.1

**Tasks:**
- [ ] **Step 1: Add coroutine support to `EventBasedActor`**

```cpp
// Add to EventBasedActor class:
#include <hpactor/sched/coroutine_task.hpp>

// Coroutine support
sched::CoroutineTask get_coroutine() const { return coroutine_; }
void set_coroutine(sched::CoroutineTask&& t) { coroutine_ = std::move(t); }

// Mailbox state for awaiter
bool mailbox_has_messages() const;
bool mailbox_is_empty() const;

// Add member:
sched::CoroutineTask coroutine_;
```

- [ ] **Step 2: Verify compilation**

```bash
ninja -C build 2>&1 | head -50
```

---

### Task 4.3: Commit Phase 4

```bash
git add src/sched/scheduler.cpp include/hpactor/actor/event_based_actor.hpp
git commit -m "feat(sched): wire coroutines into HybridScheduler execution

execute_actor() now handles coroutine resume with state transitions:
Ready→Running on pickup, Idle→Ready on re-schedule.
EventBasedActor gains get_coroutine() and mailbox state queries.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 5: Integrate TimingWheel Timer Callbacks

### Task 5.1: Make timer callbacks call `notify_ready`

**Files:**
- Modify: `src/sched/scheduler.cpp`
- Depends on: Phase 4

**Tasks:**
- [ ] **Step 1: Read current `schedule_timer()` implementation**

```bash
grep -n "schedule_timer\|timer_wheel" src/sched/scheduler.cpp
```

- [ ] **Step 2: Implement `schedule_timer()` using TimingWheel**

```cpp
uint64_t HybridScheduler::schedule_timer(int64_t delay_ns,
                                         TimingWheel::TimerCallback callback) {
    return timer_wheel_.schedule(delay_ns, std::move(callback));
}

void HybridScheduler::advance_time(int64_t now_ns) {
    timer_wheel_.advance(now_ns);
}
```

- [ ] **Step 3: Start timer advancement thread in `start()`**

```cpp
void HybridScheduler::start() {
    // ... existing worker thread startup ...

    // Start timer thread
    timer_thread_ = std::thread([this] {
        while (running_.load(std::memory_order_acquire)) {
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            timer_wheel_.advance(now);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}
```

- [ ] **Step 4: Stop timer thread in `stop()`**

```cpp
void HybridScheduler::stop() {
    running_.store(false, std::memory_order_release);
    for (auto& t : worker_threads_) {
        if (t.joinable()) t.join();
    }
    worker_threads_.clear();
    if (timer_thread_.joinable()) timer_thread_.join();
}
```

- [ ] **Step 5: Verify compilation**

```bash
ninja -C build 2>&1 | head -30
```

---

### Task 5.2: Commit Phase 5

```bash
git add src/sched/scheduler.cpp
git commit -m "feat(sched): integrate TimingWheel timer callbacks

schedule_timer() now uses TimingWheel. Timer advancement runs on
dedicated thread. Timer callbacks can call notify_ready() to
reschedule actors.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 6: Wire CoroutineFramePool into WorkerThread

### Task 6.1: Set frame pool for coroutine allocations

**Files:**
- Modify: `include/hpactor/sched/worker_thread.hpp`, `src/sched/worker_thread.cpp`
- Depends on: Phase 5

**Tasks:**
- [ ] **Step 1: Add frame pool member to WorkerThread**

Current `WorkerThread` already has `CoroutineFramePool* frame_pool_{nullptr}` and `set_frame_pool()`. Need to actually use it.

- [ ] **Step 2: Set thread-local pointer when worker starts**

```cpp
// In worker_thread.cpp
void WorkerThread::start() {
    if (frame_pool_) {
        tl_frame_pool = frame_pool_;
    }
    thread_ = std::thread([this] { thread_loop(); });
}
```

- [ ] **Step 3: Clear thread-local pointer when worker stops**

```cpp
void WorkerThread::stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    tl_frame_pool = nullptr;
}
```

- [ ] **Step 4: Verify compilation**

```bash
ninja -C build 2>&1 | head -30
```

---

### Task 6.2: Commit Phase 6

```bash
git add include/hpactor/sched/worker_thread.hpp src/sched/worker_thread.cpp
git commit -m "feat(sched): wire CoroutineFramePool into WorkerThread

Thread-local frame pool pointer set on worker start, cleared on stop.
Coroutine allocations use pool when available.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Phase 7: Create MPSCMailbox (Lock-Free)

### Task 7.1: Create `include/hpactor/mailbox/mpsc_mailbox.hpp`

**Files:**
- Create: `include/hpactor/mailbox/mpsc_mailbox.hpp`
- Depends on: Phase 6

**Tasks:**
- [ ] **Step 1: Write MPSC mailbox header**

```cpp
// include/hpactor/mailbox/mpsc_mailbox.hpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hpactor::mailbox {

// Intrusive Vyukov MPSC queue for actor mailboxes
// T must provide: std::atomic<T*> mpsc_next
template<typename T>
class MPSCMailbox {
public:
    MPSCMailbox() : head_(nullptr), tail_(nullptr), stub_(nullptr) {
        tail_.store(&stub_, std::memory_order_relaxed);
    }

    // Producer: enqueue node (wait-free)
    void enqueue(T* node) noexcept {
        node->mpsc_next.store(nullptr, std::memory_order_relaxed);
        T* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->mpsc_next.store(node, std::memory_order_release);
    }

    // Consumer: dequeue node (lock-free, single consumer)
    T* dequeue() noexcept {
        T* tail = tail_.load(std::memory_order_acquire);
        T* next = tail->mpsc_next.load(std::memory_order_acquire);

        if (tail == &stub_) {
            if (!next) return nullptr;
            tail_ = next;
            tail = next;
            next = tail->mpsc_next.load(std::memory_order_acquire);
        }

        if (next) {
            tail_.store(next, std::memory_order_release);
            return tail;
        }

        return nullptr;
    }

    // Check if empty
    bool empty() const noexcept {
        T* tail = tail_.load(std::memory_order_acquire);
        T* next = tail->mpsc_next.load(std::memory_order_acquire);
        return tail == &stub_ && !next;
    }

private:
    struct alignas(64) Node {
        std::atomic<T*> mpsc_next{nullptr};
    };

    alignas(64) std::atomic<T*> head_;
    alignas(64) T* tail_;
    Node stub_;
};

} // namespace hpactor::mailbox
```

- [ ] **Step 2: Verify compilation**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/mailbox/mpsc_mailbox.hpp -o /dev/null 2>&1
```

---

### Task 7.2: Commit Phase 7

```bash
git add include/hpactor/mailbox/mpsc_mailbox.hpp
git commit -m "feat(mailbox): add lock-free MPSCMailbox<T>

Vyukov MPSC queue: wait-free enqueue, lock-free dequeue (single consumer).
Intrusive link via atomic<T*> mpsc_next in node type.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Verification Checklist

After each phase:
- [ ] Phase 0: `IScheduler` interface matches spec; `ActorSystem` compiles
- [ ] Phase 1: Worker threads start; `try_steal()` actually steals
- [ ] Phase 2: `ActorState` CAS works; `CoroutineTask` moves correctly
- [ ] Phase 3: `MailboxAwaiter` suspends/resumes correctly
- [ ] Phase 4: Coroutines resume on `execute_actor()`; re-schedule on mailbox non-empty
- [ ] Phase 5: Timer callbacks fire; actors rescheduled
- [ ] Phase 6: Frame allocations use pool
- [ ] Phase 7: MPSC mailbox enqueue/dequeue works
- [ ] Full build: `ninja -C build` with no errors
- [ ] Full tests: `ctest --output-on-failure` all pass

---

## Dependency Graph

```
Phase 0 ─── 0.1 (IScheduler interface) ─→ 0.2 (HybridScheduler impl) ─→ 0.3 (ActorSystem wiring)
Phase 1 ─── 1.1 (start threads) ─→ 1.2 (work-stealing impl)
Phase 2 ─── 2.1 (ActorState) ─→ 2.2 (CoroutineTask)
Phase 3 ─── 3.1 (Awaiters)
Phase 4 ─── 4.1 (execute_actor) ─→ 4.2 (EventBasedActor coroutine)
Phase 5 ─── 5.1 (timer callbacks)
Phase 6 ─── 6.1 (frame pool wiring)
Phase 7 ─── 7.1 (MPSC mailbox)
```

---

## Files Summary

| File | Action | Phase |
|------|--------|-------|
| `include/hpactor/sched/scheduler.hpp` | Modify | 0, 1 |
| `src/sched/scheduler.cpp` | Modify | 0, 1, 4, 5 |
| `include/hpactor/sched/worker_thread.hpp` | Modify | 1, 6 |
| `src/sched/worker_thread.cpp` | Modify | 1, 6 |
| `include/hpactor/sched/actor_state.hpp` | Create | 2 |
| `include/hpactor/sched/coroutine_task.hpp` | Create | 2 |
| `include/hpactor/sched/coroutine_awaiters.hpp` | Create | 3 |
| `include/hpactor/actor/event_based_actor.hpp` | Modify | 4 |
| `include/hpactor/mailbox/mpsc_mailbox.hpp` | Create | 7 |
