# Worker Thread Idle Extraction & Backoff Simplification

**Date:** 2026-06-14
**Issue:** #287
**Status:** Approved

## Motivation

Two code-quality issues in `src/sched/worker_thread.cpp`:

1. **`thread_loop()` is too complex.** The "no work → poll → CV escalate" logic
   is inlined as ~65 lines inside the main loop, obscuring the loop's structure.
2. **`backoff()` uses an exponential bit-shift formula** (`10us * 2^c`) that is
   unnecessarily sophisticated. With `kYieldIters=0` and `kSleepIters=2`, only
   two sleep durations (10µs, 20µs) are ever used before CV escalation.

## Design

### 1. Extract `idle(WorkItem& item)` method

Encapsulates the entire "no work available" path:

- **Poll phase** (standalone worker OR `backoff_counter_ < kPollThreshold`):
  diagnostics++, `backoff()`, return false.
- **CV phase** (attached worker AND threshold exhausted): set `is_blocking_`,
  re-check for work (pop_local + steal), if found return true with the item;
  otherwise compute EDF-aware timeout, block on CV, record diagnostics,
  `reset_backoff()`, return false.

Return `true` = work found during CV pre-check (caller processes `item`).

**Header addition:**
```cpp
// worker_thread.hpp private section
bool idle(WorkItem& item);
```

### 2. Move sleep-tuning constants to file scope

`kSleepIters` (2) and `kPollThreshold` (2) move from `thread_loop()` locals to
file-scope `static constexpr` alongside existing `kYieldIters` (0) — needed
because `idle()` references `kPollThreshold`.

### 3. Replace exponential backoff with explicit lookup table

The `10us << (c - kYieldIters)` formula becomes a fixed array of sleep
durations. Behavior is identical for the poll iterations that matter:

| Iteration | Before (shift) | After (table) |
|-----------|---------------|---------------|
| c=0 (idx 0) | 10µs | 10µs |
| c=1 (idx 1) | 20µs | 20µs |
| c=2+ | CV takes over | CV takes over |

Defense-in-depth entries (50µs, 100µs) exist beyond index 1 for edge cases
where CV escalation is delayed (e.g. standalone workers without `owner_`).

### Preserved behavior

- Poll threshold: 2 iterations → CV (unchanged)
- Sleep durations: 10µs, 20µs (unchanged)
- CV EDF timeout logic: identical
- `is_blocking_` protocol: identical
- Diagnostic counters: all preserved
- Standalone worker path: keeps polling without CV

## Files changed

| File | Change |
|------|--------|
| `src/sched/worker_thread.cpp` | Extract `idle()`, move constants, simplify `backoff()` |
| `include/hpactor/sched/worker_thread.hpp` | Declare `idle(WorkItem&)` |
