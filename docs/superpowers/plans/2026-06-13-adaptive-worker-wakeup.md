# Adaptive Worker Wakeup — Implementation Plan

> **Goal:** Replace polling-based idle detection with adaptive escalation:
> workers poll briefly then block on CV, enqueue path notifies CV only when needed.
> EDF-aware timeout prevents deadline misses.

**Architecture:** 4 changes: WorkerState gains CV fields, enqueue_shared conditionally notifies,
thread_loop escalates from polling to CV blocking, scheduler exposes EDF deadline.

**Tech Stack:** C++20, Google Test, CMake/Ninja

---

### Task 1: Add CV Fields to WorkerState

**File:** `include/hpactor/sched/work_placement_scheduler.hpp`

Add to WorkerState struct (after `shared_input`):
```cpp
std::atomic<bool> is_blocking_{false};
std::mutex sleep_mutex_;
std::condition_variable sleep_cv_;
```

Add `#include <condition_variable>` at top.

### Task 2: CV Notify in enqueue_shared

**File:** `src/sched/work_placement_scheduler.cpp`

After the existing CAS push to shared_input, add:
```cpp
// If the target worker is blocking, wake it via CV.
if (worker.is_blocking_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lk(worker.sleep_mutex_);
    worker.is_blocking_.store(false, std::memory_order_release);
    worker.sleep_cv_.notify_one();
}
```

### Task 3: Add edf_next_deadline() to Scheduler

**File:** `include/hpactor/sched/scheduler.hpp` — declare:
```cpp
int64_t edf_next_deadline() const noexcept;
```

**File:** `src/sched/scheduler.cpp` — implement:
```cpp
int64_t HybridScheduler::edf_next_deadline() const noexcept {
    int64_t earliest = INT64_MAX;
    int64_t deadline;
    for (auto& ws : placement_.workers()) {
        if (ws.edf_queue.peek(deadline) && deadline < earliest) {
            earliest = deadline;
        }
    }
    return earliest;
}
```

### Task 4: Adaptive Worker Loop

**File:** `src/sched/worker_thread.cpp`

Replace the thread_loop's idle handling. After local pop and try_steal fail:

```cpp
// Polling phase: short backoff for burst responsiveness.
if (backoff_counter_ < 8) {
    increment_donations();
    backoff();
    continue;
}

// Escalate to blocking: CV wait with EDF-aware timeout.
auto& ws = owner_->workers()[config_.worker_index];
ws.is_blocking_.store(true, std::memory_order_seq_cst);

// Double-check: work might have arrived between last poll and flag write.
if (owner_->pop_local(item, config_.worker_index) || try_steal(item)) {
    ws.is_blocking_.store(false, std::memory_order_release);
    reset_backoff();
    if (processor_) processor_(item);
    continue;
}

// Compute EDF-aware timeout.
auto now = std::chrono::steady_clock::now();
int64_t now_ns = now.time_since_epoch().count();
int64_t edf_ns = owner_->edf_next_deadline();
auto timeout = std::chrono::milliseconds(100);
if (edf_ns != INT64_MAX) {
    int64_t delta_ns = edf_ns - now_ns;
    if (delta_ns <= 0) delta_ns = 1'000'000; // overdue: 1ms floor
    auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(delta_ns));
    timeout = std::max(delta_ms - std::chrono::milliseconds(1), std::chrono::milliseconds(1));
    if (timeout > std::chrono::milliseconds(100)) timeout = std::chrono::milliseconds(100);
}

std::unique_lock<std::mutex> lk(ws.sleep_mutex_);
ws.sleep_cv_.wait_for(lk, timeout, [&ws] {
    return !ws.is_blocking_.load(std::memory_order_relaxed);
});
reset_backoff();
```

### Task 5: Build, Test, Smoke Test

```bash
ninja -C build tests/unit/sched/test_unit_sched tests/unit/timer/test_unit_timer
./build/tests/unit/sched/test_unit_sched
./build/tests/unit/timer/test_unit_timer
ninja -C build apps/cli_demo/15_cli_demo
echo "/system stats\n/quit" | ./build/apps/cli_demo/15_cli_demo
```
