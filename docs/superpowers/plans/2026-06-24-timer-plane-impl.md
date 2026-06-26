# TimerPlane Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix 5 timer correctness bugs and build a sharded high-performance TimerPlane backend with O(1) cancel, timer groups, and full observability.

**Architecture:** Phase 1 fixes bugs directly in `TimingWheel`, `CalendarQueue`, and `HybridScheduler` timer thread. Phase 2 builds TimerPlane as a new `TimerBackend::TimerPlane` variant — N shards (one per worker), each with a slot-array for O(1) handle resolution and a per-shard timing wheel. MPSC `TimerCommandQueue` for cross-thread ops. `TimerHandle` encodes `(shard, slot, generation, type_tag)` in 64 bits. `TimerGroup` per actor for bulk cancel on stop. Metrics via new `MetricEventType` values. CLI via `/timer` commands under existing trie.

**Tech Stack:** C++20, no exceptions/RTTI, HPActor memory regions (`mem::allocate`/`mem::deallocate`), `MpscRingBuffer<MetricEvent>`, `CommandNode` CLI trie.

---

### Task 1: Bug 1 — TimingWheel insert_timer() uses wrong time for bucket-index

**Files:**
- Modify: `src/timer/timing_wheel.cpp:109-137`
- Test: `tests/unit/timer/test_timing_wheel.cpp`

- [ ] **Step 1: Write failing test — NearTermTimerFiresPromptly**

Add to `tests/unit/timer/test_timing_wheel.cpp` after `ScheduleAndFire`:

```cpp
TEST_F(TimingWheelTest, NearTermTimerFiresPromptly) {
    // Bug: insert_timer() computes bucket index from (expire_ns - current_time_)
    // instead of expire_ns directly. A near-term timer can land in the
    // already-processed bucket and wait a full rotation.

    int64_t now = current_time_ns();
    // Advance time by 5ms to move past bucket 0
    wheel_->advance(now + 5'000'000);

    bool fired = false;
    // Schedule 2ms from the new current time
    int64_t schedule_at = now + 7'000'000; // 5ms advance + 2ms delay
    auto id = wheel_->schedule_at(schedule_at, [&fired]() { fired = true; });
    ASSERT_NE(id, 0u);

    // Advance in 1ms steps past the expiry time
    int64_t step = now + 5'000'000;
    while (step <= schedule_at + 1'000'000 && !fired) {
        wheel_->advance(step);
        step += 1'000'000;
    }

    EXPECT_TRUE(fired)
        << "Timer scheduled 2ms after now should fire promptly";
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer --gtest_filter="*NearTermTimerFiresPromptly*"
```

Expected: FAIL — `fired` is false after advancing past expiry.

- [ ] **Step 3: Fix insert_timer()**

In `src/timer/timing_wheel.cpp:128`, change:
```cpp
    int64_t level_offset = now / tick_ns_;
```
to:
```cpp
    int64_t level_offset = expire / tick_ns_;
```

- [ ] **Step 4: Run tests**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer
```

Expected: ALL 14 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/timer/timing_wheel.cpp tests/unit/timer/test_timing_wheel.cpp
git commit -m "fix(timer): compute TimingWheel bucket index from expiry time

insert_timer() computed bucket from (expire_ns - current_time_) offset,
causing near-term timers to land in already-processed buckets.
Changed to compute directly from expire_ns / tick_ns_.

Added NearTermTimerFiresPromptly test.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Bug 2 — CalendarQueue advance() drops future timers

**Files:**
- Modify: `src/timer/calendar_queue.cpp:217-230`
- Create: `tests/unit/timer/test_calendar_queue.cpp`
- Modify: `tests/unit/timer/CMakeLists.txt`

- [ ] **Step 1: Create CalendarQueue test file**

Create `tests/unit/timer/test_calendar_queue.cpp` with the `CalendarQueueTest` fixture and these tests:
- `FutureTimerSurvivesAdvance` — 0ms + 7ms timers, advance 2ms → only 0ms fires, 7ms still pending
- `ScheduleAndFire` — basic schedule + advance
- `CancelRemovesTimer` — cancel prevents fire
- `EmptyAndNextDeadline` — empty wheel returns INT64_MAX

(See spec for full test code.)

- [ ] **Step 2: Update CMakeLists.txt**

Add `test_calendar_queue.cpp` to `tests/unit/timer/CMakeLists.txt`.

- [ ] **Step 3: Run test to verify failure**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer --gtest_filter="*FutureTimerSurvivesAdvance*"
```

Expected: FAIL — 7ms timer destroyed prematurely.

- [ ] **Step 4: Fix advance()**

In `src/timer/calendar_queue.cpp:220-230`, replace the loop body:

```cpp
            Timer* t = bucket.head;
            while (t) {
                Timer* next = t->next;
                bucket.unlink(t);
                if (t->expire_ns <= now_ns) {
                    timer_map_.erase(t->id);
                    pending.push_back(std::move(t->callback));
                    destroy_timer(t);
                } else {
                    // Future timer in current bucket — re-insert.
                    insert_timer(t, now_ns);
                }
                t = next;
            }
```

- [ ] **Step 5: Run tests**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer
```

Expected: ALL 18 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add src/timer/calendar_queue.cpp tests/unit/timer/test_calendar_queue.cpp tests/unit/timer/CMakeLists.txt
git commit -m "fix(timer): CalendarQueue advance() must not destroy future timers

advance() was destroying all timers in the current fine bucket regardless
of expire_ns vs now_ns. Future timers are now checked and re-inserted.

Added CalendarQueue unit test suite.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Bug 3 — CalendarQueue data races

**Files:**
- Modify: `src/timer/calendar_queue.cpp:106-113`
- Modify: `include/hpactor/adt/calendar_queue.hpp:140-150`
- Test: `tests/unit/timer/test_calendar_queue.cpp`

- [ ] **Step 1: Add TSAN stress test**

Add `ScheduleReadsTimeUnderLock` to `test_calendar_queue.cpp` — two threads running `schedule()` and `advance()` concurrently, 1000 iterations each.

- [ ] **Step 2: Run with TSAN**

```bash
cd build && cmake -DENABLE_TSAN=ON . && ninja test_unit_timer
./tests/unit/timer/test_unit_timer --gtest_filter="*ScheduleReadsTimeUnderLock*"
```

Expected: TSAN reports data race on `last_advance_ns_`.

- [ ] **Step 3: Fix schedule() — move read inside mutex**

In `src/timer/calendar_queue.cpp`, replace `schedule()`:
```cpp
uint64_t CalendarQueue::schedule(int64_t delay_ns, TimerCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = last_advance_ns_;
    int64_t expire_ns = now + delay_ns;
    if (delay_ns <= 0) expire_ns = now + fine_bucket_ns_;
    return schedule_at_locked(expire_ns, std::move(cb));
}
```

Extract existing `schedule_at()` body into private `schedule_at_locked()`. Make `schedule_at()` acquire the lock then call `schedule_at_locked()`.

- [ ] **Step 4: Fix size() — atomic counter**

Add `std::atomic<size_t> size_{0}` to `CalendarQueue` private members. Increment on insert (in `schedule_at_locked`), decrement on cancel/destroy/expire. Change `size()` to return `size_.load()`.

- [ ] **Step 5: Re-run TSAN**

```bash
./tests/unit/timer/test_unit_timer --gtest_filter="*ScheduleReadsTimeUnderLock*"
```

Expected: No TSAN warnings.

- [ ] **Step 6: Run full suite (disable TSAN)**

```bash
cd build && cmake -DENABLE_TSAN=OFF . && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer
```

Expected: ALL tests PASS.

- [ ] **Step 7: Commit**

```bash
git add src/timer/calendar_queue.cpp include/hpactor/adt/calendar_queue.hpp tests/unit/timer/test_calendar_queue.cpp
git commit -m "fix(timer): fix CalendarQueue data races in schedule() and size()

schedule() now reads last_advance_ns_ under mutex. size() uses atomic
counter instead of reading timer_map_ without lock. Added TSAN stress test.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Bug 5 — TimingWheel cancel() is O(n)

**Files:**
- Modify: `include/hpactor/timer/timing_wheel.hpp` (Timer struct, timer_map_ member)
- Modify: `src/timer/timing_wheel.cpp` (add_timer_internal, remove_timer, advance, ~TimingWheel)
- Test: `tests/unit/timer/test_timing_wheel.cpp`

- [ ] **Step 1: Add O(1) cancel tests**

Add `CancelIsConstantTime` (1000 timers, cancel last, assert < 50µs) and `CancelO1UsesMap` (cancel removes from map + bucket, second cancel returns false) to `test_timing_wheel.cpp`.

- [ ] **Step 2: Run to verify failure**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer --gtest_filter="*CancelIsConstantTime*"
```

Expected: FAIL — cancel time is proportional to wheel size.

- [ ] **Step 3: Add timer_map_ and extended Timer fields**

In `include/hpactor/timer/timing_wheel.hpp`, add to Timer struct:
```cpp
    uint32_t level;    // which wheel level this timer is in
    uint32_t bucket;   // which bucket within that level
```

Add private member:
```cpp
    std::unordered_map<uint64_t, Timer*> timer_map_;
```

Add `#include <unordered_map>`.

- [ ] **Step 4: Update add_timer_internal(), insert_timer(), remove_timer(), advance()**

In `add_timer_internal()`: after assigning `timer->id`, add `timer_map_[id] = timer;`.

In `insert_timer()`: store `timer->level = level; timer->bucket = bucket;`.

Replace `remove_timer()` with O(1) implementation:
```cpp
TimingWheel::Timer* TimingWheel::remove_timer(uint64_t timer_id) {
    auto it = timer_map_.find(timer_id);
    if (it == timer_map_.end()) return nullptr;
    Timer* timer = it->second;
    timer_map_.erase(it);
    uint32_t level = timer->level;
    uint32_t bucket = timer->bucket;
    if (level < num_levels_ && bucket < levels_[level].buckets.size()) {
        auto& vec = levels_[level].buckets[bucket];
        for (auto vit = vec.begin(); vit != vec.end(); ++vit) {
            if (*vit == timer) { vec.erase(vit); break; }
        }
    }
    return timer;
}
```

In `advance()`: when destroying expired timers, add `timer_map_.erase(timer->id);`.

- [ ] **Step 5: Run tests**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer
```

Expected: ALL 16 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/timer/timing_wheel.hpp src/timer/timing_wheel.cpp tests/unit/timer/test_timing_wheel.cpp
git commit -m "fix(timer): make TimingWheel cancel() O(1) with timer_map_

Added unordered_map for O(1) lookup. Extended Timer with level/bucket
fields for direct bucket unlinking. Added CancelIsConstantTime and
CancelO1UsesMap tests.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Bug 4 — Scheduler timer thread wakeup

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `src/sched/scheduler.cpp:85-108, 323-328`
- Create: `tests/integration/actor/test_timer_wakeup.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

- [ ] **Step 1: Create integration test**

Create `tests/integration/actor/test_timer_wakeup.cpp`:

```cpp
#include <hpactor/core/actor_system.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <atomic>
#include <thread>

namespace hpactor {
namespace {

TEST(TimerWakeupTest, ScheduleWakesSleepingTimerThread) {
    ActorSystemConfig config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    std::atomic<bool> fired{false};

    // Schedule a far-future timer so the timer thread enters a long sleep.
    auto far = system.scheduler()->schedule_after([]() {}, 10'000'000'000LL);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Schedule a short timer — must fire within ~30ms despite the long sleep.
    auto start = std::chrono::steady_clock::now();
    auto short_h = system.scheduler()->schedule_after(
        [&fired]() { fired.store(true); }, 1'000'000LL);

    auto deadline = start + std::chrono::milliseconds(100);
    while (!fired.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(fired.load());
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_LT(elapsed.count(), 30);

    system.scheduler()->cancel_timer(far);
    system.scheduler()->cancel_timer(short_h);
}

} // namespace
} // namespace hpactor
```

- [ ] **Step 2: Update CMakeLists.txt**

Add `test_timer_wakeup.cpp` to `tests/integration/actor/CMakeLists.txt`.

- [ ] **Step 3: Run to verify failure**

```bash
cd build && ninja test_integration_actor && ./build/tests/integration/actor/test_integration_actor --gtest_filter="*ScheduleWakesSleepingTimerThread*"
```

Expected: FAIL or flakes — timer takes ≥ 50ms to fire.

- [ ] **Step 4: Add condition_variable wakeup**

In `include/hpactor/sched/scheduler.hpp`, add to private members:
```cpp
    std::condition_variable timer_wakeup_cv_;
    std::mutex timer_wakeup_mutex_;
```

In `src/sched/scheduler.cpp`, replace the `sleep_for()` in the timer thread with:
```cpp
    std::unique_lock<std::mutex> lk(timer_wakeup_mutex_);
    timer_wakeup_cv_.wait_for(lk, std::chrono::nanoseconds(sleep_ns));
```

In `schedule_after()` (line 323), add after backend insertion:
```cpp
    timer_wakeup_cv_.notify_one();
```

- [ ] **Step 5: Run tests**

```bash
cd build && ninja test_integration_actor && ./build/tests/integration/actor/test_integration_actor --gtest_filter="*ScheduleWakesSleepingTimerThread*"
```

Expected: PASS — short timer fires within ~20ms.

- [ ] **Step 6: Run full existing test suite**

```bash
ctest -R "TimingWheel|CalendarQueue|schedule|Schedule|recurring" --output-on-failure
```

Expected: All PASS.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp tests/integration/actor/test_timer_wakeup.cpp tests/integration/actor/CMakeLists.txt
git commit -m "fix(sched): wake timer thread on schedule_after() for earlier deadline

Replaced sleep_for() with condition_variable::wait_for() so
schedule_after() can interrupt long sleeps for new short timers.
Added ScheduleWakesSleepingTimerThread integration test.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Foundational TimerPlane types

**Files:**
- Create: `include/hpactor/timer/timer_options.hpp`
- Create: `include/hpactor/timer/timer_node.hpp`

No separate tests — data types tested through Task 7-11.

- [ ] **Step 1: Create TimerOptions**

Create `include/hpactor/timer/timer_options.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <chrono>

namespace hpactor::sched {

struct TimerOptions {
    uint8_t priority{0};
    std::chrono::milliseconds deadline{0};
    bool propagate_trace{true};
    uint64_t tolerance_ns{0};

    static TimerOptions defaults() { return TimerOptions{}; }
    static TimerOptions high_priority() {
        TimerOptions opts;
        opts.priority = 0;
        return opts;
    }
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Create TimerNode + TimerCommand**

Create `include/hpactor/timer/timer_node.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <hpactor/sched/scheduler_interfaces.hpp>
#include <hpactor/tracing/trace_context.hpp>
#include <cstdint>

namespace hpactor::sched {

struct TimerNode {
    TimerNode* next{nullptr};
    TimerNode* prev{nullptr};
    int64_t expire_ns{0};
    uint32_t slot_index{0};
    uint8_t generation{0};
    uint64_t group_handle{0};
    uint8_t priority{0};
    TraceContext trace;
    timer_callback callback;
};

struct TimerCommand {
    enum class Type : uint8_t { Schedule = 0, Cancel = 1, DrainGroup = 2 };
    Type type{Type::Schedule};
    union {
        struct { int64_t expire_ns; TimerNode* node; } schedule;
        struct { uint64_t handle; } cancel;
        struct { uint64_t group_handle; } drain_group;
    };

    static TimerCommand make_schedule(int64_t exp, TimerNode* n) {
        TimerCommand c; c.type = Type::Schedule;
        c.schedule.expire_ns = exp; c.schedule.node = n; return c;
    }
    static TimerCommand make_cancel(uint64_t h) {
        TimerCommand c; c.type = Type::Cancel;
        c.cancel.handle = h; return c;
    }
    static TimerCommand make_drain_group(uint64_t gh) {
        TimerCommand c; c.type = Type::DrainGroup;
        c.drain_group.group_handle = gh; return c;
    }
};

} // namespace hpactor::sched
```

- [ ] **Step 3: Verify compilation**

```bash
cd build && ninja hpactor_lib
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/timer/timer_options.hpp include/hpactor/timer/timer_node.hpp
git commit -m "feat(timer): add TimerOptions, TimerNode, TimerCommand types

Foundation types for TimerPlane.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: TimerCommandQueue — lock-free bounded MPSC queue

**Files:**
- Create: `include/hpactor/timer/timer_command_queue.hpp`
- Create: `tests/unit/timer/test_timer_plane.cpp` (initial version)
- Modify: `tests/unit/timer/CMakeLists.txt`

- [ ] **Step 1: Create test file with queue tests**

Create `tests/unit/timer/test_timer_plane.cpp` with `TimerCommandQueueTest` suite:
- `PushAndDrainSingle` — push one, drain, verify
- `PushAndDrainMultiple` — 10 commands roundtrip
- `DrainToEmptyReturnsZero` — empty queue drain returns 0
- `PushFullQueueReturnsFalse` — fill to capacity, next push fails
- `DrainPreservesOrder` — FIFO order verified

- [ ] **Step 2: Run to verify compile error**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer --gtest_filter="*TimerCommandQueue*"
```

Expected: Compile error — `timer_command_queue.hpp` missing.

- [ ] **Step 3: Implement TimerCommandQueue**

Create `include/hpactor/timer/timer_command_queue.hpp` with a bounded SPSC/MPSC ring buffer:

```cpp
#pragma once
#include <hpactor/timer/timer_node.hpp>
#include <array>
#include <atomic>
#include <vector>

namespace hpactor::sched {

class TimerCommandQueue {
public:
    static constexpr size_t kCapacity = 256;

    bool try_push(TimerCommand cmd) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) % kCapacity;
        if (next == head_.load(std::memory_order_acquire)) return false;
        buffer_[tail] = cmd;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    size_t drain_all(std::vector<TimerCommand>& out) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t count = 0;
        while (head != tail) {
            out.push_back(buffer_[head]);
            head = (head + 1) % kCapacity;
            ++count;
        }
        head_.store(head, std::memory_order_release);
        return count;
    }

private:
    std::array<TimerCommand, kCapacity> buffer_{};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

} // namespace hpactor::sched
```

- [ ] **Step 4: Run tests**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer --gtest_filter="*TimerCommandQueue*"
```

Expected: ALL 5 queue tests PASS.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/timer/timer_command_queue.hpp tests/unit/timer/test_timer_plane.cpp tests/unit/timer/CMakeLists.txt
git commit -m "feat(timer): add TimerCommandQueue lock-free bounded MPSC queue

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: TimerHandle encode/decode helpers

**Files:**
- Modify: `include/hpactor/sched/scheduler_interfaces.hpp`
- Test: `tests/unit/timer/test_timer_plane.cpp`

- [ ] **Step 1: Add encoding tests**

Add `TimerHandleEncodingTest` suite to `test_timer_plane.cpp`:
- `EncodeDecodeRoundTrip` — encode shard/slot/gen/type, decode back, verify match
- `MaxValuesPreserve` — all-1s fields roundtrip
- `DefaultHandleIsInvalid` — default-constructed handle is !valid()
- `TypeTagZeroIsActorMessage` — type_tag 0 correct

- [ ] **Step 2: Run to verify failure**

```bash
./tests/unit/timer/test_unit_timer --gtest_filter="*TimerHandleEncoding*"
```

Expected: Compile error — `make_encoded()` not defined.

- [ ] **Step 3: Extend TimerHandle**

In `include/hpactor/sched/scheduler_interfaces.hpp`, replace `using TimerHandle = Id<TimerTag>;` with:

```cpp
class TimerHandle : public Id<TimerTag> {
public:
    using Id<TimerTag>::Id;

    static TimerHandle make_encoded(uint32_t shard_index, uint32_t slot_index,
                                     uint8_t generation, uint16_t type_tag) {
        uint64_t v = (static_cast<uint64_t>(type_tag) << 48) |
                     (static_cast<uint64_t>(shard_index) << 32) |
                     (static_cast<uint64_t>(generation) << 24) |
                     static_cast<uint64_t>(slot_index);
        return TimerHandle{v};
    }

    static uint32_t slot_index(TimerHandle h) {
        return static_cast<uint32_t>(h.value() & 0xFFFFFFULL);
    }
    static uint8_t generation(TimerHandle h) {
        return static_cast<uint8_t>((h.value() >> 24) & 0xFFULL);
    }
    static uint32_t shard_index(TimerHandle h) {
        return static_cast<uint32_t>((h.value() >> 32) & 0xFFFFULL);
    }
    static uint16_t type_tag(TimerHandle h) {
        return static_cast<uint16_t>((h.value() >> 48) & 0xFFFFULL);
    }
};
```

- [ ] **Step 4: Run tests + check for regressions**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer --gtest_filter="*TimerHandleEncoding*"
ctest -R "schedule|Schedule|recurring" --output-on-failure
```

Expected: All timer handle tests PASS. Existing scheduler tests PASS.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/sched/scheduler_interfaces.hpp tests/unit/timer/test_timer_plane.cpp
git commit -m "feat(timer): add TimerHandle encode/decode for TimerPlane

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: TimerPlane metrics event types

**Files:**
- Modify: `include/hpactor/metrics/metrics_event.hpp`

- [ ] **Step 1: Add timer metric enum values**

Add after `kReliableCancelled = 51`:
```cpp
    kTimerScheduled = 52,
    kTimerFired = 53,
    kTimerCancelled = 54,
    kTimerLate = 55,
    kTimerDropped = 56,
    kTimerFiringLatency = 57,
    kTimerCallbackDuration = 58,
```

- [ ] **Step 2: Verify compilation and existing tests**

```bash
cd build && ninja hpactor_lib && ninja test_unit_metrics && ./build/tests/unit/metrics/test_unit_metrics
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/metrics/metrics_event.hpp
git commit -m "feat(timer): add TimerPlane metric event types

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 10: TimerPlaneShard — per-shard wheel + slot array

**Files:**
- Create: `include/hpactor/timer/timer_plane_shard.hpp`
- Create: `src/timer/timer_plane_shard.cpp`
- Test: `tests/unit/timer/test_timer_plane.cpp`

- [ ] **Step 1: Add shard tests**

Add `TimerPlaneShardTest` fixture to `test_timer_plane.cpp`:
- `ScheduleAndAdvanceFires` — schedule 5ms timer, advance 10ms, assert fired
- `CancelPreventsFire` — schedule, cancel, advance, not fired
- `DoubleCancelReturnsFalse` — second cancel returns false
- `PendingCount` — schedule/cancel/advance updates pending count
- `MinDeadline` — schedule earlier timer updates min_deadline

- [ ] **Step 2: Run to verify failure**

```bash
./tests/unit/timer/test_unit_timer --gtest_filter="*TimerPlaneShard*"
```

Expected: Compile error — `timer_plane_shard.hpp` missing.

- [ ] **Step 3: Create header**

Create `include/hpactor/timer/timer_plane_shard.hpp` declaring `TimerPlaneShard` with:
- Constructor taking `(uint32_t shard_index, int64_t tick_ns)`
- `schedule(delay_ns, callback, group_handle, priority) → TimerHandle`
- `cancel(TimerHandle) → bool`
- `advance(now_ns) → uint32_t` (drains cmd queue + advances wheel)
- `push_command(TimerCommand) → bool`
- `min_deadline_ns()`, `pending_count()`, `now_ns()` — lock-free reads
- `scheduled_count()`, `fired_count()`, `cancelled_count()`, `late_count()`, `dropped_count()` — metrics
- `mutex()` → `std::mutex&`
- Private: slot array, free slot list, generation array, cmd queue, per-shard wheel bucket storage, atomic counters

- [ ] **Step 4: Create implementation**

Create `src/timer/timer_plane_shard.cpp` with:
- Constructor: pre-allocate slot array (64K slots) with LIFO free list
- `alloc_node()`/`free_node()` via `mem::allocate(kTimer)`/`mem::deallocate`
- `acquire_slot()`/`release_slot()` — LIFO free list, increment generation on allocate
- `schedule()`: acquire slot, alloc node, compute expire_ns, insert into per-shard buckets, update min_deadline, return encoded TimerHandle
- `cancel()`: decode handle, ABA check via slot + generation, unlink from buckets via node->prev/next, release slot, free node
- `advance()`: drain cmd queue (process Schedule/Cancel/DrainGroup), then advance per-shard wheel buckets, fire expired callbacks, emit late metric for late firings
- Per-shard wheel: use the same hierarchical algorithm as TimingWheel but directly storing TimerNode* pointers. Four levels, 256 buckets each. `insert_timer_node()`, `remove_timer_node()`, cascading on wrap.

Key: the per-shard wheel uses `TimerNode::next`/`TimerNode::prev` for doubly-linked bucket lists (like CalendarQueue's BucketList), not `std::vector`.

- [ ] **Step 5: Run tests**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer --gtest_filter="*TimerPlaneShard*"
```

Expected: ALL 5 shard tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/timer/timer_plane_shard.hpp src/timer/timer_plane_shard.cpp tests/unit/timer/test_timer_plane.cpp
git commit -m "feat(timer): add TimerPlaneShard — per-shard wheel + slot array

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 11: TimerPlane core — variant, timer thread, API

**Files:**
- Create: `include/hpactor/timer/timer_plane.hpp`
- Create: `src/timer/timer_plane.cpp`
- Modify: `include/hpactor/sched/scheduler.hpp` (add `TimerBackend::TimerPlane`, variant member)
- Modify: `src/sched/scheduler.cpp` (variant dispatch)
- Test: `tests/unit/timer/test_timer_plane.cpp`

- [ ] **Step 1: Add TimerPlane unit tests**

Add `TimerPlaneTest` fixture to `test_timer_plane.cpp` (construct TimerPlane with 2 shards):
- `ScheduleFiresCallback` — schedule via TimerPlane, advance, callback fires
- `CancelPreventsCallback` — schedule, cancel, advance, not fired
- `NextDeadline` — schedule two timers, next_deadline reflects earliest
- `Empty` — no timers → empty() returns true
- `Size` — schedule 3 → size() == 3, advance past → size() == 0
- `MultipleShardsFire` — schedule on different shards, all fire

- [ ] **Step 2: Run to verify failure**

```bash
./tests/unit/timer/test_unit_timer --gtest_filter="*TimerPlaneTest*"
```

Expected: Compile error — `timer_plane.hpp` missing.

- [ ] **Step 3: Create TimerPlane header**

Create `include/hpactor/timer/timer_plane.hpp`:

```cpp
#pragma once
#include <hpactor/timer/timer_plane_shard.hpp>
#include <hpactor/timer/timer_options.hpp>
#include <hpactor/sched/scheduler_interfaces.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>

namespace hpactor::sched {

class TimerPlane {
public:
    TimerPlane(uint32_t num_shards, int64_t tick_ns = 1'000'000);
    ~TimerPlane();

    // Backend API (compatible with TimingWheel/CalendarQueue)
    uint64_t schedule(int64_t delay_ns, timer_callback cb);
    uint64_t schedule_at(int64_t expire_ns, timer_callback cb);
    bool cancel(uint64_t timer_id);
    uint32_t advance(int64_t now_ns);

    // Queries
    int64_t next_deadline() const;
    bool empty() const;
    size_t size() const;

    // Start/stop the timer thread
    void start();
    void stop();

    // Per-shard access (for metrics, CLI)
    TimerPlaneShard& shard(uint32_t i);
    uint32_t num_shards() const;

private:
    void timer_loop();
    uint32_t select_shard();

    std::vector<TimerPlaneShard> shards_;
    std::atomic<bool> running_{false};
    std::thread timer_thread_;
    std::condition_variable wakeup_cv_;
    std::mutex wakeup_mutex_;
};

} // namespace hpactor::sched
```

- [ ] **Step 4: Create TimerPlane implementation**

Create `src/timer/timer_plane.cpp`:

```cpp
#include <hpactor/timer/timer_plane.hpp>
#include <algorithm>
#include <chrono>

namespace hpactor::sched {

TimerPlane::TimerPlane(uint32_t num_shards, int64_t tick_ns) {
    shards_.reserve(num_shards);
    for (uint32_t i = 0; i < num_shards; ++i) {
        shards_.emplace_back(i, tick_ns);
    }
}

TimerPlane::~TimerPlane() { stop(); }

uint32_t TimerPlane::select_shard() {
    // Hash the calling thread id for shard selection.
    auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return static_cast<uint32_t>(tid % shards_.size());
}

uint64_t TimerPlane::schedule(int64_t delay_ns, timer_callback cb) {
    uint32_t si = select_shard();
    auto& shard = shards_[si];
    TimerHandle h = shard.schedule(delay_ns, std::move(cb), 0, 0);

    // Wake the timer thread for new earlier deadlines.
    if (h.valid()) {
        wakeup_cv_.notify_one();
        return h.value();
    }
    return 0;
}

uint64_t TimerPlane::schedule_at(int64_t expire_ns, timer_callback cb) {
    int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    return schedule(std::max(expire_ns - now, int64_t{0}), std::move(cb));
}

bool TimerPlane::cancel(uint64_t timer_id) {
    TimerHandle h{timer_id};
    uint32_t si = TimerHandle::shard_index(h);
    if (si >= shards_.size()) return false;
    return shards_[si].cancel(h);
}

uint32_t TimerPlane::advance(int64_t now_ns) {
    uint32_t total = 0;
    for (auto& shard : shards_) {
        total += shard.advance(now_ns);
    }
    return total;
}

int64_t TimerPlane::next_deadline() const {
    int64_t min_dl = INT64_MAX;
    for (auto& shard : shards_) {
        min_dl = std::min(min_dl, shard.min_deadline_ns());
    }
    return min_dl;
}

bool TimerPlane::empty() const {
    for (auto& shard : shards_) {
        if (shard.pending_count() > 0) return false;
    }
    return true;
}

size_t TimerPlane::size() const {
    size_t total = 0;
    for (auto& shard : shards_) total += shard.pending_count();
    return total;
}

void TimerPlane::start() {
    running_.store(true);
    timer_thread_ = std::thread([this] { timer_loop(); });
}

void TimerPlane::stop() {
    running_.store(false);
    wakeup_cv_.notify_one();
    if (timer_thread_.joinable()) timer_thread_.join();
}

void TimerPlane::timer_loop() {
    while (running_.load(std::memory_order_acquire)) {
        int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        advance(now);

        int64_t next_ns = next_deadline();
        int64_t sleep_ns = (next_ns == INT64_MAX)
            ? 100'000'000LL
            : std::clamp(next_ns - now, 1'000'000LL, 100'000'000LL);

        std::unique_lock<std::mutex> lk(wakeup_mutex_);
        wakeup_cv_.wait_for(lk, std::chrono::nanoseconds(sleep_ns));
    }
}

TimerPlaneShard& TimerPlane::shard(uint32_t i) { return shards_[i]; }
uint32_t TimerPlane::num_shards() const { return static_cast<uint32_t>(shards_.size()); }

} // namespace hpactor::sched
```

- [ ] **Step 5: Integrate into scheduler variant**

In `include/hpactor/sched/scheduler.hpp`, update the `TimerBackend` enum:
```cpp
enum class TimerBackend : uint8_t {
    TimingWheel = 0,
    CalendarQueue = 1,
    TimerPlane = 2,
};
```

Update the variant:
```cpp
std::variant<TimingWheel, CalendarQueue, TimerPlane> timer_backend_;
```

In `src/sched/scheduler.cpp`, add `#include <hpactor/timer/timer_plane.hpp>`.

In the constructors where the variant is built, add the `TimerPlane` case (`TimerPlane` constructor takes `num_workers` as shard count).

In the timer thread, `advance_time()`, `schedule_timer()`: the `std::visit` already handles the new variant automatically. For the timer thread startup, conditionally call `timer_backend_.emplace<TimerPlane>(num_workers_)` then `std::get<TimerPlane>(timer_backend_).start()`... 

Actually, TimerPlane manages its own timer thread. When TimerPlane is selected, the scheduler should NOT start its own timer thread. Rather than complex conditional logic, make `TimerPlane::start()`/`stop()` integrate with the scheduler's existing thread model.

Simpler approach: TimerPlane does NOT manage its own thread. The scheduler's existing timer thread loop calls `advance()` and reads `next_deadline()` via `std::visit`, same as it does for TimingWheel/CalendarQueue. TimerPlane's `schedule()` already calls `wakeup_cv_.notify_one()`, making the existing scheduler wakeup (from Task 5) work. The scheduler timer thread uses the scheduler's own `timer_wakeup_cv_`, not TimerPlane's.

So TimerPlane is a passive backend — no thread of its own. The `start()`/`stop()` methods are removed. The `wakeup_cv_` in TimerPlane is removed — the scheduler's `schedule_after()` handles wakeup centrally.

- [ ] **Step 6: Update scheduler for TimerPlane backend**

In `src/sched/scheduler.cpp`, the constructor handles three backends:
```cpp
switch (config_.timer_backend) {
    case TimerBackend::TimingWheel:
        timer_backend_.emplace<TimingWheel>(1'000'000, 4);
        break;
    case TimerBackend::CalendarQueue:
        timer_backend_.emplace<CalendarQueue>(CalendarQueueConfig{...});
        break;
    case TimerBackend::TimerPlane:
        timer_backend_.emplace<TimerPlane>(num_workers_, 1'000'000);
        break;
}
```

No changes to `schedule_after()`, `cancel_timer()`, `advance_time()` — `std::visit` handles all three backends uniformly. The timer thread loop is unchanged — wakeup is handled by the scheduler's `timer_wakeup_cv_` from Task 5.

- [ ] **Step 7: Run TimerPlane unit tests**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer --gtest_filter="*TimerPlaneTest*"
```

Expected: ALL 6 TimerPlane tests PASS.

- [ ] **Step 8: Run existing tests for regressions**

```bash
cd build && ninja && ctest --output-on-failure --parallel 8
```

Expected: All 2105+ tests PASS.

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/timer/timer_plane.hpp src/timer/timer_plane.cpp include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp tests/unit/timer/test_timer_plane.cpp
git commit -m "feat(timer): add TimerPlane core — sharded timer backend

New TimerBackend::TimerPlane variant with per-worker shards, slot-array
O(1) handle resolution, MPSC command queues. Passive backend — uses
scheduler's existing timer thread and wakeup mechanism.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 12: TimerGroup — bulk cancel for actor lifecycle

**Files:**
- Create: `include/hpactor/timer/timer_group.hpp`
- Create: `src/timer/timer_group.cpp`
- Test: `tests/unit/timer/test_timer_plane.cpp`

- [ ] **Step 1: Add TimerGroup tests**

Add `TimerGroupTest` to `test_timer_plane.cpp`:
- `AddAndRemoveHandle` — add handles, remove one, verify size
- `CancelAllInvokesCancel` — create TimerGroup, add 3 handles, cancel_all removes all
- `EmptyGroupCancelAllReturnsZero` — cancel_all on empty group = 0
- `GroupHandleEncoding` — TimerGroupHandle roundtrip

- [ ] **Step 2: Implement TimerGroup**

Create `include/hpactor/timer/timer_group.hpp`:
```cpp
#pragma once
#include <hpactor/types/types.hpp>
#include <unordered_set>
#include <functional>
#include <cstdint>

namespace hpactor::sched {

class TimerPlane; // fwd decl

class TimerGroup {
public:
    void add(uint64_t handle);
    void remove(uint64_t handle);
    size_t cancel_all(TimerPlane& plane);
    size_t size() const;
    bool empty() const;
private:
    std::unordered_set<uint64_t> handles_;
};

} // namespace hpactor::sched
```

Create `src/timer/timer_group.cpp` implementing `add`, `remove`, `cancel_all` (iterates handles, calls `plane.cancel(h)` for each, clears set).

- [ ] **Step 3: Run tests**

```bash
cd build && ninja test_unit_timer && ./tests/unit/timer/test_unit_timer --gtest_filter="*TimerGroup*"
```

Expected: ALL 4 TimerGroup tests PASS.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/timer/timer_group.hpp src/timer/timer_group.cpp tests/unit/timer/test_timer_plane.cpp
git commit -m "feat(timer): add TimerGroup for bulk timer cancellation

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 13: ActorContext API extensions

**Files:**
- Modify: `include/hpactor/actor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`

- [ ] **Step 1: Extend schedule() signatures**

In `actor_context.hpp`, add overloads with `TimerOptions`:
```cpp
#include <hpactor/timer/timer_options.hpp>

AlarmHandle schedule(std::chrono::milliseconds delay, TypedMessage msg,
                     TimerOptions opts);
AlarmHandle schedule_to(const ActorAddress& target,
                        std::chrono::milliseconds delay, TypedMessage msg,
                        TimerOptions opts);
```

The existing overloads (without `TimerOptions`) remain unchanged for backward compatibility.

- [ ] **Step 2: Implement extensions**

In `actor_context.cpp`, implement the new overloads. They follow the same pattern as the existing `schedule()`/`schedule_to()` but pass `opts` through. When TimerPlane is not the active backend, `TimerOptions` fields are unused but harmless.

- [ ] **Step 3: Run existing tests**

```bash
cd build && ninja && ctest -R "schedule|Schedule" --output-on-failure
```

Expected: All existing schedule tests PASS. Backward compatible.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/actor_context.hpp src/actor/actor_context.cpp
git commit -m "feat(timer): add TimerOptions parameter to ActorContext::schedule()

Backward compatible — existing signatures unchanged.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 14: CLI timer commands

**Files:**
- Create: `src/cli/commands/timer_commands.cpp`
- Modify: `src/cli/CMakeLists.txt` (if needed)

- [ ] **Step 1: Create timer commands**

Create `src/cli/commands/timer_commands.cpp` with self-registering commands:
- `/timer stats` — aggregate pending/fired/cancelled/late/dropped across all shards, next deadline
- `/timer stats <shard>` — per-shard detail: pending, cmd queue depth, counters
- `/timer inspect <handle>` — resolve handle: shard, slot, gen, expire, group, type
- `/timer groups` — list all actor timer groups with counts
- `/timer groups <actor_id>` — per-actor detail: pending handles, expire times

Uses the `InspectStateRequest`/`InspectStateReply` path for accessing TimerPlane state from within the CLI actor (non-blocking, thread-safe via message passing). Adds a `TimerStatsRequest`/`TimerStatsReply` pair or extends the existing `InspectStateRequest` with a `TimerStats` variant.

For the initial implementation, use a simpler approach: add `TimerStatsSnapshot` struct that TimerPlane can fill in lock-free, exposed via `ActorSystem::timer_stats()`.

- [ ] **Step 2: Build and test**

```bash
cd build && ninja
./build/apps/cli_demo   # manual smoke test with /timer stats
```

- [ ] **Step 3: Commit**

```bash
git add src/cli/commands/timer_commands.cpp
git commit -m "feat(cli): add /timer stats, inspect, and groups commands

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 15: Integration tests — TimerPlane with actors

**Files:**
- Create: `tests/integration/actor/test_timer_plane_actor.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

- [ ] **Step 1: Create integration tests**

Create `tests/integration/actor/test_timer_plane_actor.cpp`:

Tests:
- `ActorScheduleThroughTimerPlane` — actor schedules message, it arrives in mailbox
- `CancelPreventsActorDelivery` — cancel after schedule, no message
- `TimerGroupCancelOnActorStop` — actor stop → pending timers cancelled via group
- `CrossThreadScheduleToRemoteShard` — schedule from thread A to actor on thread B's shard
- `LateFiringMetricEmitted` — timer fires late, kTimerLate counter increments

- [ ] **Step 2: Run integration tests**

```bash
cd build && ninja test_integration_actor && ./build/tests/integration/actor/test_integration_actor --gtest_filter="*TimerPlane*"
```

Expected: ALL 5 integration tests PASS.

- [ ] **Step 3: Commit**

```bash
git add tests/integration/actor/test_timer_plane_actor.cpp tests/integration/actor/CMakeLists.txt
git commit -m "test(timer): add TimerPlane actor integration tests

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 16: System tests — end-to-end TimerPlane

**Files:**
- Create: `tests/system/test_timer_plane_system.cpp`
- Modify: `tests/system/CMakeLists.txt`

- [ ] **Step 1: Create system tests**

Create `tests/system/test_timer_plane_system.cpp`:

Tests:
- `TimerPlaneBackendEndToEnd` — full ActorSystem configured with TimerPlane backend, schedule messages, verify delivery
- `GracefulShutdownPendingTimers` — shutdown with pending timers, group cancel invoked, no leaks
- `MetricsEndpointReturnsTimerMetrics` — scrape /metrics, verify timer gauges/counters present

- [ ] **Step 2: Run system tests**

```bash
cd build && ninja && ctest -R "timer_plane_system" --output-on-failure
```

Expected: ALL 3 system tests PASS.

- [ ] **Step 3: Commit**

```bash
git add tests/system/test_timer_plane_system.cpp tests/system/CMakeLists.txt
git commit -m "test(timer): add TimerPlane system tests

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 17: Full verification

- [ ] **Step 1: Build everything**

```bash
cd build && ninja
```

Expected: Zero compile errors.

- [ ] **Step 2: Run all tests**

```bash
ctest --output-on-failure --parallel 8
```

Expected: All 2105+ existing tests PASS + all new timer tests PASS.

- [ ] **Step 3: Run with ASAN**

```bash
cd build && cmake -DENABLE_ASAN=ON . && ninja
ctest -R "timer|Timer" --output-on-failure
```

Expected: No ASAN errors.

- [ ] **Step 4: Verify git status**

```bash
git status
git diff --stat main
```

---

## File Manifest

### Files Created

| File | Task |
|------|------|
| `include/hpactor/timer/timer_options.hpp` | 6 |
| `include/hpactor/timer/timer_node.hpp` | 6 |
| `include/hpactor/timer/timer_command_queue.hpp` | 7 |
| `include/hpactor/timer/timer_plane_shard.hpp` | 10 |
| `include/hpactor/timer/timer_plane.hpp` | 11 |
| `include/hpactor/timer/timer_group.hpp` | 12 |
| `src/timer/timer_plane_shard.cpp` | 10 |
| `src/timer/timer_plane.cpp` | 11 |
| `src/timer/timer_group.cpp` | 12 |
| `src/cli/commands/timer_commands.cpp` | 14 |
| `tests/unit/timer/test_calendar_queue.cpp` | 2 |
| `tests/unit/timer/test_timer_plane.cpp` | 7, 8, 10, 11, 12 |
| `tests/integration/actor/test_timer_wakeup.cpp` | 5 |
| `tests/integration/actor/test_timer_plane_actor.cpp` | 15 |
| `tests/system/test_timer_plane_system.cpp` | 16 |

### Files Modified

| File | Task |
|------|------|
| `src/timer/timing_wheel.cpp` | 1, 4 |
| `include/hpactor/timer/timing_wheel.hpp` | 4 |
| `src/timer/calendar_queue.cpp` | 2, 3 |
| `include/hpactor/adt/calendar_queue.hpp` | 3 |
| `include/hpactor/sched/scheduler.hpp` | 5, 11 |
| `src/sched/scheduler.cpp` | 5, 11 |
| `include/hpactor/sched/scheduler_interfaces.hpp` | 8 |
| `include/hpactor/metrics/metrics_event.hpp` | 9 |
| `include/hpactor/actor/actor_context.hpp` | 13 |
| `src/actor/actor_context.cpp` | 13 |
| `tests/unit/timer/test_timing_wheel.cpp` | 1, 4 |
| `tests/unit/timer/CMakeLists.txt` | 2, 7 |
| `tests/integration/actor/CMakeLists.txt` | 5, 15 |
| `tests/system/CMakeLists.txt` | 16 |
