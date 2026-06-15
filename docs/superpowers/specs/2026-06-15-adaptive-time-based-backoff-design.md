# Adaptive Time-Based Worker Backoff Design

**Date**: 2026-06-15  
**Issue**: [#303](https://github.com/skg7on/HPActor/issues/303)  
**Status**: Design approved, awaiting implementation plan

## Motivation

The worker thread backoff in `src/sched/worker_thread.cpp` uses platform-specific magic
numbers — `kBackoffYieldIters` (0 on Linux, 4 on macOS) and `kBackoffSleepIters` (4) —
that encode empirical observations about `sched_yield` behavior on each OS. These
constants do not adapt to kernel configuration (`CONFIG_HZ`), hrtimer granularity,
system load, or workload burst patterns.

On Linux (kernel 6.8.0-124), even with light message workloads, a worker thread in the
CV blocking model consumes **~0.6% CPU** when the ideal is near 0%.

### Root Causes

1. **Fixed 100ms CV timeout when no EDF deadlines exist.** With no EDF work, the
   worker wakes up 10 times/second. Each wake cycle costs ~600 µs of CPU (mutex,
   seq_cst stores, EDF scan across all workers, pop-local double-checks).
   600 µs × 10 = 6 ms/s = 0.6%.

2. **Iteration-count-based backoff, not wall-clock-based.** After each CV timeout
   wakeup the backoff counter resets to 0 and the worker re-runs the 4-iteration
   nanosleep ramp (150 µs total) before re-entering CV — even though the system has
   already been idle for 100 ms+ and should skip straight back to deep sleep.

3. **No adaptation to workload history.** The mechanism does not distinguish "just
   became idle" from "has been idle for minutes." A truly idle system should quickly
   reach deep CV sleep with a long timeout.

## Design

Three additions to the worker thread idle path. The existing three-phase structure
(`try_find_and_process_work` → `try_poll_idle` → `enter_cv_block`) is preserved.

### 1. OS Calibration — Startup Probe

A `BackoffCalibration` struct is populated once, when the first worker thread starts,
by running a short probe sequence on that worker's OS thread.

```
struct BackoffCalibration {
    bool     yield_is_effective;       // false on Linux/CFS, true on macOS/Mach
    uint32_t min_effective_sleep_ns;   // kernel timer granularity (typ. 10–50 µs)
    uint32_t spin_threshold_ns;        // how long to spin before first sleep
    uint32_t polling_budget_ns;        // total time in polling before CV escalation
};
```

**Probe sequence** (runs once, guarded by `std::call_once`):

1. **Yield effectiveness.** Time 1000 consecutive `sched_yield()` calls.
   - If wall-clock duration is close to pure-CPU-spin time → yield does not actually
     yield (Linux CFS) → `yield_is_effective = false`.
   - If significantly longer (ns per yield) → yield works (macOS Mach) →
     `yield_is_effective = true`.

2. **Timer granularity.** Sleep for 1, 10, 50, 100, 500, 1000 µs and measure actual
   elapsed time for each. The knee where `actual ÷ requested` stabilizes is
   `min_effective_sleep_ns`. On modern Linux with hrtimers this is ~10–50 µs; on
   older kernels it may be 1–4 ms.

3. **Derived thresholds:**
   - `spin_threshold_ns = yield_is_effective ? 20'000 : 0` (skip spin on Linux).
   - `polling_budget_ns = max(10'000'000, min_effective_sleep_ns * 100)` (~10 ms).

#### Test Injection

A static pointer `const BackoffCalibration* test_calibration_override_` allows
tests to inject fixed calibration values before `start()`. When set, the probe is
skipped and the injected values are used. This keeps timing-dependent backoff
behavior deterministic in tests.

```cpp
// Test usage:
BackoffCalibration cal;
cal.yield_is_effective = false;
cal.min_effective_sleep_ns = 50'000;
cal.spin_threshold_ns = 0;
cal.polling_budget_ns = 10'000'000;
WorkerThread::set_test_calibration(&cal);
// ... test ...
WorkerThread::set_test_calibration(nullptr);
```

### 2. Time-Based Idle Stages

The iteration-count-based `backoff()` is replaced with a wall-clock-based version
that takes elapsed idle time as input.

**New state** on `WorkerThread`:

```
std::chrono::steady_clock::time_point idle_since_;  // set on first idle iter, cleared on work
```

**`try_poll_idle()` logic:**

```cpp
bool WorkerThread::try_poll_idle() {
    auto now = std::chrono::steady_clock::now();

    // Record idle start on first idle iteration.
    // idle_since_ defaults to epoch; a non-epoch value means we are tracking idle.
    if (idle_since_ == std::chrono::steady_clock::time_point{}) {
        idle_since_ = now;
    }

    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - idle_since_).count();

    // Stay in polling while within the polling budget.
    if (static_cast<uint32_t>(elapsed_ns) < calibration_.polling_budget_ns) {
        diag_idle_iters_.fetch_add(1, std::memory_order_relaxed);
        increment_donations();
        backoff(now - idle_since_);
        return true;  // continue polling
    }
    return false;  // escalate to CV
}
```

**`reset_backoff()`** sets `idle_since_ = std::chrono::steady_clock::time_point{}`
(epoch sentinel, meaning "not idle").  This is called when work is found and after
CV wakeup so the idle clock restarts on next idle encounter.

**`backoff(elapsed)` — proportional sleep stages:**

`elapsed` is a `std::chrono::nanoseconds` value (time since `idle_since_`).

| Stage | Condition | Action |
|-------|-----------|--------|
| 0 | `elapsed < spin_threshold_ns` | `yield()` if `yield_is_effective`, else no-op |
| 1 | `elapsed < 1'000'000 ns` (1 ms) | `nanosleep(elapsed / 4)`, floored at `min_effective_sleep_ns` |
| 2 | `elapsed ≥ 1'000'000 ns` (1 ms) | `nanosleep(500'000 ns)` (500 µs cap before next poll iteration) |

Key difference from current: **no fixed iteration count.** The number of backoff
iterations depends on how long each sleep actually takes, which in turn depends on
OS timer granularity. On a system with 50 µs minimum sleep, ~200 iterations fit in
the 10 ms polling budget. On a system with 1 ms granularity, ~10 iterations. Either
way, the thread spends roughly the same wall-clock time in polling before committing
to deep CV sleep.

### 3. Exponential CV Timeout

The fixed 100 ms default CV timeout is replaced with an exponential schedule when no
EDF deadlines exist.

**New state** on `WorkerThread`:

```
uint32_t consecutive_empty_wakes_;  // reset on work, incremented on empty timeout wake
```

**Timeout computation in `enter_cv_block()`:**

```cpp
auto timeout = std::chrono::milliseconds(100);  // default

if (edf_ns != INT64_MAX) {
    // EDF-aware: wake 1 ms before earliest deadline, capped at 100 ms.
    // (unchanged from current behavior)
    int64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    int64_t delta_ns = edf_ns - now_ns;
    if (delta_ns <= 0) delta_ns = 1'000'000;
    auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(delta_ns));
    auto margin = std::chrono::milliseconds(1);
    timeout = (delta_ms > margin) ? (delta_ms - margin) : std::chrono::milliseconds(1);
    if (timeout > std::chrono::milliseconds(100))
        timeout = std::chrono::milliseconds(100);
} else {
    // No EDF deadlines: exponential safety-net timeout.
    // Doubles each empty wake, capped at 30 seconds.
    constexpr uint32_t kBaseTimeoutMs = 100;
    constexpr uint32_t kMaxTimeoutMs = 30'000;
    uint32_t shift = std::min(consecutive_empty_wakes_, 9u);  // 2^9 * 100ms > 30s
    uint32_t ms = kBaseTimeoutMs << shift;
    timeout = std::chrono::milliseconds(std::min(ms, kMaxTimeoutMs));
}
```

**`consecutive_empty_wakes_` management:**

- Reset to 0 in `process_work_item()` — work was found, system is active.
- Reset to 0 on notify-wake (`diag_cv_notify_wakes_` path) where work was found.
- Incremented on timeout-wake (`diag_cv_timeout_wakes_` path) where no work was found.

| Empty wakes | Timeout | Effect |
|-------------|---------|--------|
| 0 | 100 ms | Fresh idle — check back soon |
| 1 | 200 ms | Still idle — double |
| 2 | 400 ms | |
| 3 | 800 ms | |
| 4 | 1.6 s | |
| 5 | 3.2 s | |
| 6 | 6.4 s | |
| 7 | 12.8 s | |
| 8+ | 30 s (cap) | Deep idle — near-zero CPU |

**After ~8 empty cycles (~1 minute idle), CPU usage drops to near zero.**

**No latency penalty for work arrival.** The `enqueue_shared()` → `wake_if_blocking()`
path notifies the CV immediately regardless of timeout, so the worker wakes as soon
as work is pushed to its shared-input stack. The exponential timeout only affects
the safety-net path.

## State Changes Summary

New `WorkerThread` members:

| Member | Type | Purpose |
|--------|------|---------|
| `calibration_` | `BackoffCalibration` | Immutable after startup, used by all backoff decisions |
| `idle_since_` | `steady_clock::time_point` | Wall-clock time when worker first became idle |
| `consecutive_empty_wakes_` | `std::atomic<uint32_t>` | Counter for exponential CV timeout growth; atomic for snapshot reads from CLI/metrics threads |

Removed `WorkerThread` members:

| Member | Reason |
|--------|--------|
| `backoff_counter_` (the atomic for iterations) | Replaced by `idle_since_` wall-clock tracking |

File-scope constants removed:

| Constant | Reason |
|----------|--------|
| `kBackoffYieldIters` | Replaced by `calibration_.spin_threshold_ns` |
| `kBackoffSleepIters` | Replaced by `calibration_.polling_budget_ns` |
| `kPollThreshold` | Replaced by `calibration_.polling_budget_ns` comparison |

## Diagnostics

New fields in `WorkerSnapshot`:

| Field | Type | Purpose |
|-------|------|---------|
| `calibration_yield_effective` | `bool` | Whether `sched_yield` actually yields on this OS |
| `calibration_min_sleep_ns` | `uint32_t` | Measured kernel timer granularity |
| `consecutive_empty_wakes` | `uint32_t` | Current exponential timeout level |
| `idle_duration_us` | `uint64_t` | Elapsed wall-clock time since last work (0 if active) |

Existing diagnostics (`diag_cv_escalations`, `diag_cv_notify_wakes`,
`diag_cv_timeout_wakes`, `diag_work_found`, `diag_idle_iters`) are preserved.

## Testing

### Unit Tests (new)

- Calibration probe: `yield_is_effective` is correct for the current platform.
- Calibration probe: `min_effective_sleep_ns` is within sane bounds (1 µs – 10 ms).
- With injected calibration, backoff sleep durations match expected stages.
- `consecutive_empty_wakes_` increments on timeout-wake with no work.
- `consecutive_empty_wakes_` resets to 0 when work is found.
- CV timeout grows per the exponential schedule and caps at 30 s.
- After a CV timeout wake with no work, the worker skips backoff and re-enters CV
  (no wasted ramp).

### Integration Tests (existing, should still pass)

- `WorkerThreadTest.*` — all existing tests must pass unchanged.
- `test_hybrid_scheduler.cpp` — scheduler-level tests must pass.
- `test_priority_scheduler.cpp` — priority scheduling tests must pass.

## Acceptance Criteria

- [ ] Idle CPU usage on Linux ≤ 0.05% (down from ~0.6%) with light workload.
- [ ] No latency regression for work arrival via `wake_if_blocking()`.
- [ ] Calibration probe runs once on first worker start; cost < 10 ms.
- [ ] Deterministic tests can inject calibration via `test_calibration_override_`.
- [ ] All existing tests pass on both Linux and macOS.
- [ ] CI passes (ninja + ctest).
