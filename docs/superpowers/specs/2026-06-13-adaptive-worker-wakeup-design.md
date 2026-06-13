# Adaptive Worker Wakeup — Design

Date: 2026-06-13

## Motivation

The scheduler uses pure polling: workers and the timer thread wake on fixed
intervals to check for work, regardless of actual demand. Prior fixes reduced
polling frequency but couldn't eliminate it because no mechanism exists to
wake a sleeping worker when work is enqueued.

This design adds adaptive wakeup: workers poll when busy (responsive to bursts),
then escalate to condition-variable blocking when idle (zero CPU).

## Design

### State Machine

```
POLLING (c=0..7)            BLOCKING (c>=8)
┌─────────────────┐         ┌──────────────────────────┐
│ yield ×4 (c 0-3) │         │ is_blocking_ = true      │
│ sleep µs (c 4-7) │──c>=8──▶│ double-check for work    │
│                  │         │ wait_for(CV, deadline)   │
│ work found: c=0  │◀───────│ work found: c=0          │
└─────────────────┘         └──────────────────────────┘
```

**Polling phase** (c=0..7, ~300µs total): identical to current behavior — yield, then
10-640µs sleeps. Handles burst workloads with zero added overhead on the enqueue
hot path.

**Blocking phase** (c>=8): worker sets `is_blocking_ = true`, double-checks for
work (covers the race between the last poll and the flag write), then blocks on
a condition variable. The CV timeout is the EDF deadline (capped at 100ms) so
the worker wakes in time to process EDF work from any other worker.

### Enqueue Path (zero overhead when polling)

```cpp
void enqueue_shared(item, priority, worker_id) {
    // ... existing CAS push to shared_input stack ...

    if (worker.is_blocking_.load(acquire)) {  // single atomic load
        std::lock_guard lock(worker.sleep_mutex_);
        worker.is_blocking_.store(false, release);
    }
    worker.sleep_cv_.notify_one();
}
```

When the worker is polling, `is_blocking_` is false → the branch predicts
false → zero extra work. When the worker is blocking, the CV is notified
and the worker wakes.

### Worker Loop

```cpp
// After existing polls (pop_local, try_steal) return nothing:

if (backoff_counter_ < 8) {
    backoff();                               // yield or µs sleep
    return;
}

// Escalate to blocking
auto& ws = owner_->workers()[worker_index];
ws.is_blocking_.store(true, release);

// Double-check for work that arrived between last poll and flag write
if (pop_local(item) || try_steal(item)) {
    ws.is_blocking_.store(false, release);
    reset_backoff();
    process(item);
    continue;
}

// Block until woken by enqueue or EDF deadline
auto deadline = owner_->edf_next_deadline();
auto timeout = (deadline == INT64_MAX)
    ? std::chrono::milliseconds(100)
    : std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::nanoseconds(deadline - now)) - margin;
if (timeout < 1ms) timeout = 1ms;

std::unique_lock lock(ws.sleep_mutex_);
ws.sleep_cv_.wait_for(lock, timeout, [&ws] {
    return !ws.is_blocking_.load(relaxed);
});
reset_backoff();
```

### EDF-Aware Timeout

`HybridScheduler::edf_next_deadline()` peeks at all per-worker EDF queues via
`peek()` and returns the earliest deadline (or `INT64_MAX` if all empty).

This ensures: if a worker holding EDF work goes idle (blocking), another worker
wakes before the EDF deadline, steals the work via `try_steal()`, and processes
it on time.

### Timer Thread

Same approach — already sleeps until `next_deadline()` via the prior fix.
Add CV notification from `schedule()` when a new timer is earlier than the
current wait, for tighter timer accuracy.

### Per-Worker State

Add to `WorkPlacementScheduler::WorkerState`:
```cpp
std::atomic<bool> is_blocking_{false};
std::mutex sleep_mutex_;
std::condition_variable sleep_cv_;
```

### Behavior by Load

| Load | Worker state | is_blocking_ | enqueue path | CPU |
|------|-------------|--------------|-------------|-----|
| Idle | Blocks on CV | true | notify CV (~10µs) | Near-zero |
| Low (~65/s) | Polls briefly, then CV | mostly false | atomic load only | <0.1% |
| High (100K+/s) | Pure polling | false | atomic load only | Busy (correct) |
| Burst | CV wake → poll | transient | notify on 1st msg | Adaptive |

## Files Changed

| File | Change |
|------|--------|
| `include/hpactor/sched/work_placement_scheduler.hpp` | Add is_blocking_, sleep_mutex_, sleep_cv_ to WorkerState |
| `src/sched/work_placement_scheduler.cpp` | Notify CV in enqueue_shared when worker is blocking |
| `include/hpactor/sched/scheduler.hpp` | Declare edf_next_deadline() |
| `src/sched/scheduler.cpp` | Implement edf_next_deadline(); timer thread CV notify |
| `src/sched/worker_thread.cpp` | Replace backoff loop with adaptive escalation using CV |

5 files, ~100 lines.
