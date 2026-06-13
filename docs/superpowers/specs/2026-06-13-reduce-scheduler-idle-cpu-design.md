# Reduce Scheduler Idle CPU — Design

Date: 2026-06-13

## Motivation

The `cli_demo` app runs at ~45 messages/sec across 4 scheduler threads, yet consumes
0.1%–1% CPU. Root cause: the scheduler uses a polling architecture where threads
wake at ~1ms intervals regardless of workload.

Three targeted fixes eliminate wasted wakeups without adding architectural complexity.

---

## Fix 1: Timer Thread — Sleep Until Next Deadline

### Current behavior

`scheduler.cpp:84-93`: the timer thread calls `advance(now)` then unconditionally
`sleep_for(1ms)` — 1000 wakeups/sec even when no timer is due for 100ms+.

### Fix

Add `next_deadline()` to both timer backends so the timer thread computes the
optimal sleep duration:

```cpp
int64_t next_ns = backend.next_deadline();
int64_t sleep_ns = std::min(next_ns - now, 100'000'000L);
if (sleep_ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
```

### `TimingWheel::next_deadline()`

Iterate all buckets at all levels, find minimum `expire_ns`. Return `INT64_MAX`
when the wheel is empty. O(1024 buckets) — acceptable for a once-per-loop check
on the non-latency-critical timer thread.

### `CalendarQueue::next_deadline()`

Iterate fine/coarse/remote wheels, find minimum `expire_ns`. Return `INT64_MAX`
when empty.

### Impact

From ~1000 wakeups/sec → only wakes when timers actually fire (~10/sec for cli_demo).

---

## Fix 2: Worker Backoff — Raise Cap to 50ms

### Current behavior

`worker_thread.cpp:176` — backoff caps at 1024µs. Idle workers poll 1000×/sec
even when there's no work.

### Fix

Raise `1024u` to `50000u` (50ms). The backoff ramp is unchanged below the cap:
yield ×4 → 10µs → 20µs → ... → 50ms.

### Trade-off

Worst-case latency to pick up cross-worker work increases from ~1ms to ~50ms.
Acceptable: timer callbacks deliver messages immediately on the timer thread;
the worker polling delay only matters for work-stealing from other workers.

### Impact

Idle worker wakeups drop from ~1000/sec to ~20/sec per thread.

---

## Scope

| File | Change |
|------|--------|
| `include/hpactor/timer/timing_wheel.hpp` | Declare `next_deadline()` |
| `src/timer/timing_wheel.cpp` | Implement `next_deadline()` |
| `include/hpactor/adt/calendar_queue.hpp` | Declare `next_deadline()` |
| `src/timer/calendar_queue.cpp` | Implement `next_deadline()` |
| `src/sched/scheduler.cpp` | Timer thread uses `next_deadline()` |
| `src/sched/worker_thread.cpp` | Raise backoff cap to 50ms |

6 files, ~55 lines. No new files, no config changes, no API breaks.
