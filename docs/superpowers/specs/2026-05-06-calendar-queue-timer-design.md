# Calendar Queue Timer Manager — Design Specification

**Date:** 2026-05-06
**Status:** Draft (revised after spec review)

---

## 1. Overview

Replace the existing `TimingWheel` as the default timer backend with a new `CalendarQueue` implementing a hierarchical calendar queue algorithm. The `TimingWheel` is preserved — `HybridScheduler` selects the backend at construction time via a `TimerBackend` enum.

**Goal:** O(1) insert, cancel, and per-timer tick cost across a wide range of timer durations (sub-millisecond to hours).

## 2. Motivation

The current `TimingWheel` has two non-O(1) operations:

- **`remove_timer()`** (used by `cancel`): walks buckets linearly to find a timer by ID. Timer IDs encode the level in high bits as a partial optimization, but the search within a level's buckets is still O(bucket_items).
- **`insert_timer()`**: computes the level via a loop over `num_levels` and traverses cascaded offsets.

The CalendarQueue fixes both with:
- A `std::unordered_map<uint64_t, Timer*>` for O(1) id-to-node lookup on cancel.
- Direct bucket computation via integer division + bitmask — no loops.

## 3. Algorithm

### 3.1 Three-Level Hierarchical Calendar

Three levels provide coverage from 1ms to ~4.5 hours without the O(n) cascade problem
that single-last-bucket clamping would cause for bulk long-lived timers.

```
Fine wheel (near-term, high resolution):
  bucket_width = 1ms
  num_buckets  = 256      (power of 2)
  total range  = 256ms

Coarse wheel (mid-term):
  bucket_width = 256ms    (fine_bucket_width × fine_buckets)
  num_buckets  = 256
  total range  = ~65s

Remote wheel (far-future, low resolution):
  bucket_width = 65s      (coarse_bucket_width × coarse_buckets)
  num_buckets  = 256
  total range  = ~4.6 hours
```

**Insert (bucket = absolute time / bucket_width & mask):**

```
expire relative to now:

  expire < now + 256ms?
    ──yes──> Fine:   bucket = (expire / fine_width) & fine_mask

  expire < now + 65s?
    ──yes──> Coarse: bucket = (expire / coarse_width) & coarse_mask

  else ──> Remote:   bucket = (expire / remote_width) & remote_mask
```

Timers use the natural modulo of their absolute expiration time against the
appropriate wheel size. A timer at `now + 70s` maps to `(T / 65s) & 255` in the
remote wheel — not clamped to a single bucket. This distributes long timers
across all 256 remote buckets, so each coarse-cycle cascade processes at most
`total_timers / 256` timers rather than all of them.

### 3.2 Tick (advance)

`advance()` processes **all** fine buckets from `last_advance_ns_` to `now_ns`,
handling arbitrary time deltas (system suspend, scheduler lag). Pseudocode:

```
while (last_advance_ns_ + fine_bucket_ns <= now_ns):
    // 1. Fire all expired timers in current fine bucket
    for timer in fine_wheel[current_fine_]:
        unlink from bucket
        remove from timer_map_
        if not cancelled:
            fire callback         // with lock HELD (recursive_mutex)
        delete timer

    // 2. Advance fine pointer
    last_advance_ns_ += fine_bucket_ns
    current_fine_ = (current_fine_ + 1) & fine_mask

    // 3. On fine wrap: cascade one coarse bucket
    if current_fine_ == 0:
        for timer in coarse_wheel[current_coarse_]:
            unlink from bucket
            if T < now + fine_range → insert into fine wheel
            else → re-insert into coarse wheel (closer bucket)
        current_coarse_ = (current_coarse_ + 1) & coarse_mask

        // 4. On coarse wrap: cascade one remote bucket
        if current_coarse_ == 0:
            for timer in remote_wheel[current_remote_]:
                unlink from bucket
                if T < now + coarse_range → insert into coarse wheel
                else → re-insert into remote wheel (closer bucket)
            current_remote_ = (current_remote_ + 1) & remote_mask
```

Each cascade step re-evaluates the timer against the now-current time and places
it in the appropriate (closer) wheel. A 5-minute timer in the remote wheel is
re-examined once per ~65s — after ~4 cascades it reaches the fine wheel and
fires within 256ms accuracy.

**Capping:** To avoid unbounded mutex hold time under extreme time jumps
(e.g., system suspend for hours), `advance()` processes at most 4096 fine
buckets (~4 seconds) per call. If more buckets need processing, the timer thread
will pick them up on the next 1ms wakeup.

### 3.3 Cancel

`std::unordered_map<uint64_t, Timer*>` provides O(1) id-to-node lookup. Cancel
unlinks the node from its intrusive linked list, removes it from the map, and
frees it immediately — no lazy sweeping, no deferred cleanup.

### 3.4 Recurring Timers

Handled identically to the current `TimingWheel`: the callback re-schedules
itself via `schedule()`. The `HybridScheduler::schedule_every()` wrapper
(cancellation flag + self-rescheduling lambda) is backend-agnostic and unchanged.

## 4. Data Structures

### 4.1 Timer Node

```cpp
struct Timer : mem::SlabAllocated<Timer> {
    int64_t expire_ns;       // absolute expiration
    uint64_t id;             // unique, assigned by CalendarQueue
    TimerCallback callback;
    Timer* next = nullptr;   // intrusive doubly-linked list
    Timer* prev = nullptr;
    uint32_t bucket_idx = 0; // for debug assertions
    uint8_t wheel_level = 0; // 0=fine, 1=coarse, 2=remote
};
```

Uses `mem::SlabAllocated<Timer>` for consistency with the existing
`TimingWheel::Timer`, routing allocations through the framework's two-tier slab
allocator.

### 4.2 Buckets

```cpp
struct BucketList {
    Timer* head = nullptr;
    Timer* tail = nullptr;
    uint32_t count = 0;

    void push_back(Timer* t);
    void unlink(Timer* t);    // O(1) — doubly-linked
    Timer* pop_front();
};
```

### 4.3 CalendarQueue

```cpp
class CalendarQueue {
public:
    struct Config {
        int64_t fine_bucket_ns = 1'000'000;    // 1ms
        uint32_t fine_buckets = 256;            // power of 2
        uint32_t coarse_buckets = 256;          // power of 2
        uint32_t remote_buckets = 256;          // power of 2
        uint32_t max_advance_buckets = 4096;    // ~4s cap per advance() call
    };

private:
    std::vector<BucketList> fine_wheel_;    // [fine_buckets]
    std::vector<BucketList> coarse_wheel_;  // [coarse_buckets]
    std::vector<BucketList> remote_wheel_;  // [remote_buckets]
    std::unordered_map<uint64_t, Timer*> timer_map_;

    int64_t fine_bucket_ns_;
    int64_t coarse_bucket_ns_;   // fine_bucket_ns * fine_buckets
    int64_t remote_bucket_ns_;   // coarse_bucket_ns * coarse_buckets
    uint32_t fine_mask_;
    uint32_t coarse_mask_;
    uint32_t remote_mask_;
    uint32_t max_advance_buckets_;

    uint32_t current_fine_ = 0;
    uint32_t current_coarse_ = 0;
    uint32_t current_remote_ = 0;
    int64_t last_advance_ns_ = 0;

    std::atomic<uint64_t> next_id_{1};

    // Recursive: advance() fires callbacks that may call schedule()/cancel()
    mutable std::recursive_mutex mutex_;
};
```

`std::vector<BucketList>` provides RAII for bucket arrays. Bucket counts are
enforced as powers of 2 via `static_assert` in the constructor (the defaults
256/256/256 satisfy this). Bucket index is always computed via `& mask`, never
`%`.

## 5. Interface

```cpp
using TimerCallback = std::function<void()>;

explicit CalendarQueue(const Config& cfg = {});
~CalendarQueue();   // frees all remaining Timer nodes, callbacks NOT fired

[[nodiscard]] uint64_t schedule(int64_t delay_ns, TimerCallback cb);
[[nodiscard]] uint64_t schedule_at(int64_t expire_ns, TimerCallback cb);
bool cancel(uint64_t timer_id);    // true if found and cancelled
uint32_t advance(int64_t now_ns);  // returns number fired this call

bool empty() const;
size_t size() const;               // timer_map_.size()
```

Timer IDs are always >= 1. ID 0 is reserved (matches `TimerHandle::valid()`).
`TimerCallback` is a standalone type alias — identical to `sched::timer_callback`
in `scheduler.hpp` line 56. Both `TimingWheel::TimerCallback` and
`CalendarQueue::TimerCallback` are compatible with `std::visit` generic lambdas.

## 6. Scheduler Integration

### 6.1 TimerBackend Enum

```cpp
enum class TimerBackend : uint8_t { TimingWheel = 0, CalendarQueue = 1 };
```

### 6.2 HybridScheduler Changes

- Constructor takes `TimerBackend` parameter (default `TimingWheel`).
- Private member: `std::variant<TimingWheel, CalendarQueue> timer_backend_`.
- `schedule_after`, `schedule_every`, `cancel_timer`, `advance_time` dispatch via `std::visit`.
- `schedule_timer()` (used by `TimerAwaiter` in coroutine_awaiters.hpp): parameter type changed from `TimingWheel::TimerCallback` to the standalone `sched::timer_callback`. Dispatches through `std::visit`.
- `IScheduler` interface is unchanged.

### 6.3 ActorSystem Config

```cpp
struct Config {
    // ...
    sched::TimerBackend timer_backend = sched::TimerBackend::TimingWheel;
};
```

## 7. Concurrency

### 7.1 Thread Model

- **Timer thread** (owned by `HybridScheduler`): calls `advance()` once per ~1ms.
  Fires callbacks synchronously while holding the mutex.
- **Actor threads** (worker pool): may call `schedule()` / `cancel()` from any
  thread. These calls acquire the same mutex.

### 7.2 Recursive Mutex

`std::recursive_mutex` is used because `advance()` holds the lock while firing
callbacks, and those callbacks may call `schedule()` (e.g., recurring timer
self-reschedule in `HybridScheduler::schedule_every()`) or `cancel()`. Without
recursive locking, this is a self-deadlock.

### 7.3 Lifecycle Guarantee

`HybridScheduler::stop()` joins all worker threads **before** joining the timer
thread (existing behavior, `scheduler.cpp:82-94`). This means after `stop()`
completes, no actor threads can call `schedule()` or `cancel()`, and the timer
thread is the sole owner of the CalendarQueue. No use-after-free is possible.

## 8. Edge Cases

| Scenario | Handling |
|----------|----------|
| Timer with 0 or negative delay | Clamp to `now + fine_bucket_ns` |
| Timer beyond remote range (>4.6h) | Natural modulo into remote wheel; cascaded on each 65s coarse cycle, reaches fine wheel after ~4 coarse cycles |
| Cancel non-existent id | Return false, no-op |
| Cancel already-fired timer | Timer removed from map before callback fires; cancel returns false |
| Advance with `now_ns < last_advance_ns_` | No-op, return 0 |
| Empty queue on advance | No-op, return 0 |
| Advance after large time jump (>4s) | Process at most `max_advance_buckets_` (4096) fine buckets; remaining deferred to subsequent calls |
| Callback throws (shouldn't happen — `-fno-exceptions`) | Undefined; callbacks are assumed noexcept |
| `schedule()` called during `advance()` callback | Safe: recursive_mutex allows re-acquire |
| `cancel()` called during `advance()` callback | Safe: unlinks from bucket, removes from map, marks deleted; `advance()` loop skips already-removed timers |

## 9. Test Plan

| Test | What It Validates |
|------|-------------------|
| `test_calendar_basic_schedule` | Schedule one-shot, advance past it, callback fires |
| `test_calendar_cancel` | Schedule then cancel, advance past, callback does not fire |
| `test_calendar_cancel_during_advance` | Callback cancels another timer; no deadlock, correct behavior |
| `test_calendar_fine_coarse_split` | Schedule timer at 300ms (coarse), advance in 1ms steps, fires at correct time |
| `test_calendar_fine_coarse_remote` | Schedule timer at 70s (remote), verify cascade path, fires at correct time |
| `test_calendar_cascade` | Fill one coarse bucket, verify cascade into fine on wrap |
| `test_calendar_remote_cascade` | Fill one remote bucket, verify cascade into coarse on coarse wrap |
| `test_calendar_recurring` | Schedule recurring, verify periodic firing, cancel stops it |
| `test_calendar_recurring_no_deadlock` | Recurring timer self-reschedules; advance does not deadlock |
| `test_calendar_many_timers` | 10k timers, fire all, verify count and no leaks |
| `test_calendar_time_jump` | Advance by 5s in one call; all intermediate timers fire correctly |
| `test_calendar_empty_advance` | Advance on empty queue, no-op |
| `test_calendar_time_backwards` | Advance with now < previous, no-op |
| `test_calendar_size_and_empty` | Verify size() and empty() after insert/cancel/fire |
| `test_calendar_id_zero_invalid` | Schedule returns id >= 1; TimerHandle(id=0).valid() is false |

## 10. File Changes

| File | Action | Purpose |
|------|--------|---------|
| `include/hpactor/sched/calendar_queue.hpp` | Create | CalendarQueue class |
| `src/sched/calendar_queue.cpp` | Create | Implementation |
| `include/hpactor/sched/scheduler.hpp` | Modify | Add `TimerBackend` enum, variant member, fix `schedule_timer` callback type |
| `src/sched/scheduler.cpp` | Modify | Dispatch through variant, pass backend to constructor |
| `include/hpactor/core/actor_system.hpp` | Modify | Add `timer_backend` to `Config` |
| `include/hpactor/sched/coroutine_awaiters.hpp` | Modify | Update `schedule_timer` callback type reference |
| `tests/sched/test_calendar_queue.cpp` | Create | Unit tests |
| `tests/sched/CMakeLists.txt` | Modify | Add test target |
