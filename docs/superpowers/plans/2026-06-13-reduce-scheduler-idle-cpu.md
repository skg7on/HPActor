# Reduce Scheduler Idle CPU — Implementation Plan

> **Goal:** Eliminate unnecessary CPU wakeups by making the timer thread sleep until next deadline and raising the worker backoff cap.

**Architecture:** Two independent changes: timer backends gain `next_deadline()`, scheduler timer thread computes optimal sleep; worker backoff cap raised from 1ms to 50ms.

**Tech Stack:** C++20, Google Test, CMake/Ninja

---

### Task 1: Add `next_deadline()` to TimingWheel

**Files:**
- Modify: `include/hpactor/timer/timing_wheel.hpp` — declare method
- Modify: `src/timer/timing_wheel.cpp` — implement
- Test: `tests/unit/timer/test_unit_timer` — verify next_deadline

- [ ] **Step 1: Write failing test**

In `tests/unit/timer/test_timing_wheel.cpp` (find correct file first), add:

```cpp
TEST(TimingWheelTest, NextDeadlineEmpty) {
    TimingWheel wheel(1'000'000, 4);
    EXPECT_EQ(wheel.next_deadline(), INT64_MAX);
}

TEST(TimingWheelTest, NextDeadlineWithTimers) {
    TimingWheel wheel(1'000'000, 4);
    auto now = wheel.current_time();
    wheel.schedule(5'000'000, []{});  // 5ms
    wheel.schedule(10'000'000, []{}); // 10ms
    int64_t nd = wheel.next_deadline();
    EXPECT_GE(nd, now + 5'000'000);
    EXPECT_LE(nd, now + 5'000'000 + 1'000'000); // within 1 tick
}
```

- [ ] **Step 2: Verify test fails to compile**

```bash
ninja -C build tests/unit/timer/test_unit_timer 2>&1 | grep "next_deadline"
```
Expected: `no member named 'next_deadline'`

- [ ] **Step 3: Implement**

In `timing_wheel.hpp`, after `empty()` declaration:
```cpp
    int64_t next_deadline() const;
```

In `timing_wheel.cpp`, add:
```cpp
int64_t TimingWheel::next_deadline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t min_expire = INT64_MAX;
    for (const auto& level : levels_) {
        for (const auto& bucket : level.buckets) {
            for (const Timer* timer : bucket) {
                if (timer->expire_ns < min_expire) {
                    min_expire = timer->expire_ns;
                }
            }
        }
    }
    return min_expire;
}
```

- [ ] **Step 4: Verify test passes**

```bash
ninja -C build tests/unit/timer/test_unit_timer && ./build/tests/unit/timer/test_unit_timer --gtest_filter="*NextDeadline*"
```
Expected: PASS

- [ ] **Step 5: Commit**

---

### Task 2: Add `next_deadline()` to CalendarQueue

**Files:**
- Modify: `include/hpactor/adt/calendar_queue.hpp` — declare method
- Modify: `src/timer/calendar_queue.cpp` — implement

- [ ] **Step 1: Implement** (test in existing suite if available, else verify by build)

In `calendar_queue.hpp`, after `empty()` declaration:
```cpp
    int64_t next_deadline() const;
```

In `calendar_queue.cpp`, add:
```cpp
int64_t CalendarQueue::next_deadline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t min_expire = INT64_MAX;
    for (const auto& [id, timer] : timer_map_) {
        if (timer->expire_ns < min_expire) {
            min_expire = timer->expire_ns;
        }
    }
    return min_expire;
}
```

- [ ] **Step 2: Verify compiles**

```bash
ninja -C build tests/unit/timer/test_unit_timer
```

- [ ] **Step 3: Commit**

---

### Task 3: Timer thread sleeps until next deadline

**Files:**
- Modify: `src/sched/scheduler.cpp:84-93`

Replace the timer thread loop with:

```cpp
    timer_thread_ = std::thread([this] {
        while (running_.load(std::memory_order_acquire)) {
            if (!workers_paused_.load(std::memory_order_acquire)) {
                auto now = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
                // Sleep until the next timer deadline, capping at 100ms
                // so the thread stays responsive to stop/shutdown.
                int64_t next_ns = INT64_MAX;
                std::visit(
                    [&](auto& backend) { next_ns = backend.next_deadline(); },
                    timer_backend_);
                int64_t sleep_ns = std::min(next_ns - now, 100'000'000L);
                if (sleep_ns > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::nanoseconds(sleep_ns));
                }
                now = std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count();
                std::visit([&](auto& backend) { backend.advance(now); },
                           timer_backend_);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });
```

- [ ] **Step 2: Build and run scheduler tests**

```bash
ninja -C build tests/unit/sched/test_unit_sched && ./build/tests/unit/sched/test_unit_sched
```

- [ ] **Step 3: Commit**

---

### Task 4: Raise worker backoff cap to 50ms

**Files:**
- Modify: `src/sched/worker_thread.cpp:176`

Change:
```cpp
    if (backoff_us > 1024u) {
        backoff_us = 1024u;
    }
```
To:
```cpp
    if (backoff_us > 50000u) {
        backoff_us = 50000u;
    }
```

- [ ] **Step 1: Build and run scheduler tests**

```bash
ninja -C build tests/unit/sched/test_unit_sched && ./build/tests/unit/sched/test_unit_sched
```

- [ ] **Step 2: Commit**

---

### Task 5: Full build verification

```bash
ninja -C build apps/cli_demo/15_cli_demo
echo -e "/system stats\n/quit" | ./build/apps/cli_demo/15_cli_demo 2>&1 | head -30
```
