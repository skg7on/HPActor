# Calendar Queue Timer Manager — Design Specification

**Date:** 2026-05-06
**Status:** Draft

---

## 1. Overview

Replace the existing `TimingWheel` as the default timer backend with a new `CalendarQueue` implementing the classic Brown '88 hierarchical calendar queue algorithm. The `TimingWheel` is preserved — `HybridScheduler` selects the backend at construction time via a `TimerBackend` enum.

**Goal:** O(1) insert, cancel, and per-timer tick cost across a wide range of timer durations (sub-millisecond to hours).

## 2. Motivation

The current `TimingWheel` has two non-O(1) operations:

- **`remove_timer()`** (used by `cancel`): walks buckets linearly to find a timer by ID. Timer IDs encode the level in high bits as a partial optimization, but the search within a level's buckets is still O(bucket_items).
- **`insert_timer()`**: computes the level via a loop over `num_levels` and traverses cascaded offsets.

The CalendarQueue fixes both with:
- A `std::unordered_map<uint64_t, Timer*>` for O(1) id-to-node lookup on cancel.
- Direct bucket computation via integer division + bitmask — no loops.

## 3. Algorithm

### 3.1 Two-Level Hierarchical Calendar

```
Fine wheel (near-term, high resolution):
  bucket_width = 1ms
  num_buckets  = 256
  total range  = 256ms

Coarse wheel (far-future, low resolution):
  bucket_width = 256ms  (fine bucket_width × fine buckets)
  num_buckets  = 256
  total range  = ~65s
```

**Insert timeline:**
```
timer T relative to now:

  T < now + 256ms?
    ──yes──> Fine:   bucket = (T / 1ms) % 256
    ──no───> Coarse: bucket = (T / 256ms) % 256
              (beyond 65s → clamped to last coarse bucket)
```

**Tick (advance fine pointer by one fine bucket):**
```
1. Process all timers in fine_wheel[current_fine_bucket]:
     for each timer:
       if timer.expired: fire callback, delete
       else: keep in list (handled on next rotation)
2. If current_fine_bucket wrapped to 0:
     Cascade one coarse bucket into the fine wheel:
       for each timer in coarse_wheel[current_coarse_bucket]:
         if T < now + 256ms → insert into fine wheel
         else → re-insert into coarse wheel (closer bucket)
     current_coarse_bucket = (current_coarse_bucket + 1) % 256
```

**Timers beyond coarse range** (e.g., 5 minutes) sit in the last coarse bucket. Each 65s cycle they cascade one step closer, eventually reaching the fine wheel. This is amortized O(1) since long timers are rare.

### 3.2 Cancel

`std::unordered_map<uint64_t, Timer*>` provides O(1) id-to-node lookup. Cancel unlinks the node from its intrusive linked list and frees it immediately — no lazy sweeping, no deferred cleanup.

### 3.3 Recurring Timers

Handled identically to the current `TimingWheel`: the callback re-schedules itself via `schedule()`. The `HybridScheduler::schedule_every()` wrapper (cancellation flag + self-rescheduling lambda) is backend-agnostic and unchanged.

## 4. Data Structures

### 4.1 Timer Node

```cpp
struct Timer {
    int64_t expire_ns;       // absolute expiration
    uint64_t id;             // unique, assigned by CalendarQueue
    TimerCallback callback;
    Timer* next = nullptr;   // intrusive linked list
    Timer* prev = nullptr;

    // Which bucket does this timer live in (for debug assertions)
    uint32_t bucket_idx = 0;
    bool is_fine = true;
};
```

### 4.2 Buckets

```cpp
struct BucketList {
    Timer* head = nullptr;
    Timer* tail = nullptr;
    uint32_t count = 0;

    void push_back(Timer* t);
    void unlink(Timer* t);   // O(1) — doubly-linked
    Timer* pop_front();
};
```

### 4.3 CalendarQueue

```cpp
class CalendarQueue {
    BucketList* fine_wheel_;     // [fine_buckets_]
    BucketList* coarse_wheel_;   // [coarse_buckets_]
    std::unordered_map<uint64_t, Timer*> timer_map_;

    int64_t fine_bucket_ns_;
    uint32_t fine_buckets_;
    uint32_t coarse_buckets_;
    uint32_t fine_mask_;          // fine_buckets_ - 1
    uint32_t coarse_mask_;        // coarse_buckets_ - 1

    uint32_t current_fine_ = 0;
    uint32_t current_coarse_ = 0;
    int64_t last_advance_ns_ = 0;

    std::atomic<uint64_t> next_id_{1};
};
```

## 5. Interface

```cpp
struct CalendarConfig {
    int64_t fine_bucket_ns = 1'000'000;   // 1ms
    uint32_t fine_buckets = 256;
    uint32_t coarse_buckets = 256;
};

using TimerCallback = std::function<void()>;

CalendarQueue(const CalendarConfig& cfg = {});
~CalendarQueue();

uint64_t schedule(int64_t delay_ns, TimerCallback cb);
uint64_t schedule_at(int64_t expire_ns, TimerCallback cb);
bool cancel(uint64_t timer_id);
uint32_t advance(int64_t now_ns);   // returns number fired

bool empty() const;
size_t size() const;                // timer_map_.size()
```

Same signatures as `TimingWheel`, so `HybridScheduler` dispatches through `std::variant`.

## 6. Scheduler Integration

### 6.1 TimerBackend Enum

```cpp
enum class TimerBackend : uint8_t { TimingWheel = 0, CalendarQueue = 1 };
```

### 6.2 HybridScheduler Changes

- Constructor takes `TimerBackend` parameter (default `TimingWheel`).
- Private member: `std::variant<TimingWheel, CalendarQueue> timer_backend_`.
- `schedule_after`, `schedule_every`, `cancel_timer`, `advance_time` dispatch via `std::visit`.
- `IScheduler` interface is unchanged.

### 6.3 ActorSystem Config

```cpp
struct Config {
    // ...
    sched::TimerBackend timer_backend = sched::TimerBackend::TimingWheel;
};
```

## 7. Concurrency

The CalendarQueue is **single-threaded** — same constraint as `TimingWheel`. All operations happen on the timer advancement thread. `HybridScheduler::schedule_after()` calls `timer_backend_.schedule()` from potentially arbitrary threads, but this is serialized by the scheduler's internal design (timer inserts are queued or the calendar itself is only accessed under the scheduler's lock).

Actually — the current `TimingWheel` is called directly from `HybridScheduler::schedule_after()`, which can be called from any actor thread. This is a pre-existing concurrency concern. For the `CalendarQueue`, we apply the same approach: `schedule()` and `cancel()` are thread-safe via a simple `std::mutex` protecting bucket mutations and the timer map. The `advance()` method runs on the timer thread and acquires the same mutex.

```cpp
mutable std::mutex mutex_;
```

## 8. Edge Cases

| Scenario | Handling |
|----------|----------|
| Timer with 0 or negative delay | Clamp to now + 1 fine bucket |
| Timer beyond coarse range | Place in last coarse bucket, cascade on each coarse cycle |
| Cancel non-existent id | Return false, no-op |
| Cancel already-fired timer | Timer removed from map during fire before callback; cancel returns false |
| Advance called with time going backwards | No-op, return 0 |
| Empty queue on advance | No-op, return 0 |
| Callback throws (shouldn't happen — `-fno-exceptions`) | Undefined; callbacks are assumed noexcept |

## 9. Test Plan

| Test | What It Validates |
|------|-------------------|
| `test_calendar_basic_schedule` | Schedule one-shot, advance past it, callback fires |
| `test_calendar_cancel` | Schedule then cancel, advance past, callback does not fire |
| `test_calendar_fine_coarse_split` | Schedule timer at 300ms (coarse), advance in 1ms steps, fires at correct time |
| `test_calendar_cascade` | Fill one coarse bucket, verify cascade into fine on wrap |
| `test_calendar_beyond_coarse` | Schedule 5-minute timer, advance through multiple coarse cycles |
| `test_calendar_recurring` | Schedule recurring, verify periodic firing, cancel stops it |
| `test_calendar_many_timers` | 10k timers, fire all, verify count and no leaks |
| `test_calendar_empty_advance` | Advance on empty queue, no-op |
| `test_calendar_time_backwards` | Advance with now < previous, no-op |
| `test_calendar_size_and_empty` | Verify size() and empty() after insert/cancel/fire |

## 10. File Changes

| File | Action | Purpose |
|------|--------|---------|
| `include/hpactor/sched/calendar_queue.hpp` | Create | CalendarQueue class |
| `src/sched/calendar_queue.cpp` | Create | Implementation |
| `include/hpactor/sched/scheduler.hpp` | Modify | Add `TimerBackend` enum, variant member |
| `src/sched/scheduler.cpp` | Modify | Dispatch through variant, pass backend to constructor |
| `include/hpactor/core/actor_system.hpp` | Modify | Add `timer_backend` to `Config` |
| `tests/sched/test_calendar_queue.cpp` | Create | Unit tests |
| `tests/sched/CMakeLists.txt` | Modify | Add test target |
