# Calendar Queue Timer Manager — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a CalendarQueue with O(1) insert/cancel/tick as an alternative timer backend alongside the existing TimingWheel, selectable at HybridScheduler construction time.

**Architecture:** Three-level hierarchical calendar (fine/coarse/remote wheels) with `std::unordered_map<uint64_t, Timer*>` for O(1) cancel, `std::recursive_mutex` for re-entrant callback safety, and `std::variant<TimingWheel, CalendarQueue>` dispatch in HybridScheduler. Bucket arrays use `std::vector<BucketList>` for RAII. Timer nodes use `mem::SlabAllocated<Timer>` for consistency with the existing slab allocator.

**Tech Stack:** C++20, `std::variant`, `std::recursive_mutex`, `std::unordered_map`, intrusive linked lists, `mem::SlabAllocated<T>`, CMake + Ninja.

**Spec:** `docs/superpowers/specs/2026-05-06-calendar-queue-timer-design.md`

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `include/hpactor/sched/calendar_queue.hpp` | Create | CalendarQueue class, Config struct, Timer, BucketList |
| `src/sched/calendar_queue.cpp` | Create | Implementation |
| `include/hpactor/sched/scheduler.hpp` | Modify | Add `TimerBackend` enum, variant member, fix `schedule_timer` type |
| `src/sched/scheduler.cpp` | Modify | Constructor backend select, `std::visit` dispatch |
| `include/hpactor/core/actor_system.hpp` | Modify | Add `timer_backend` to `Config` |
| `include/hpactor/sched/coroutine_awaiters.hpp` | Modify | Update `schedule_timer` callback type |
| `tests/sched/test_calendar_queue.cpp` | Create | 15 unit tests |
| `tests/CMakeLists.txt` | Modify | Add test target |

---

### Task 1: Create CalendarQueue Header

**Files:**
- Create: `include/hpactor/sched/calendar_queue.hpp`

- [ ] **Step 1: Write the CalendarQueue header**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/mem/std_allocator.hpp>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hpactor::sched {

class CalendarQueue {
public:
    using TimerCallback = std::function<void()>;

    struct Config {
        int64_t fine_bucket_ns = 1'000'000;     // 1ms
        uint32_t fine_buckets = 256;             // power of 2
        uint32_t coarse_buckets = 256;           // power of 2
        uint32_t remote_buckets = 256;           // power of 2
        uint32_t max_advance_buckets = 4096;     // ~4s cap per advance()
    };

    explicit CalendarQueue(const Config& cfg = {});
    ~CalendarQueue();

    CalendarQueue(const CalendarQueue&) = delete;
    CalendarQueue& operator=(const CalendarQueue&) = delete;
    CalendarQueue(CalendarQueue&&) = delete;
    CalendarQueue& operator=(CalendarQueue&&) = delete;

    [[nodiscard]] uint64_t schedule(int64_t delay_ns, TimerCallback cb);
    [[nodiscard]] uint64_t schedule_at(int64_t expire_ns, TimerCallback cb);
    bool cancel(uint64_t timer_id);
    uint32_t advance(int64_t now_ns);

    bool empty() const;
    size_t size() const { return timer_map_.size(); }

private:
    struct Timer : mem::SlabAllocated<Timer> {
        int64_t expire_ns;
        uint64_t id;
        TimerCallback callback;
        Timer* next = nullptr;
        Timer* prev = nullptr;
        uint32_t bucket_idx = 0;
        uint8_t wheel_level = 0;  // 0=fine, 1=coarse, 2=remote
    };

    struct BucketList {
        Timer* head = nullptr;
        Timer* tail = nullptr;
        uint32_t count = 0;

        void push_back(Timer* t);
        void unlink(Timer* t);
    };

    // Re-insert a timer at the appropriate wheel level given current time
    void insert_timer(Timer* timer, int64_t now_ns);

    // Cascade one bucket from the given level down to the next finer level
    void cascade_coarse(int64_t now_ns);
    void cascade_remote(int64_t now_ns);

    std::vector<BucketList> fine_wheel_;
    std::vector<BucketList> coarse_wheel_;
    std::vector<BucketList> remote_wheel_;
    std::unordered_map<uint64_t, Timer*> timer_map_;

    int64_t fine_bucket_ns_;
    int64_t coarse_bucket_ns_;
    int64_t remote_bucket_ns_;
    uint32_t fine_mask_;
    uint32_t coarse_mask_;
    uint32_t remote_mask_;
    uint32_t max_advance_buckets_;

    uint32_t current_fine_ = 0;
    uint32_t current_coarse_ = 0;
    uint32_t current_remote_ = 0;
    int64_t last_advance_ns_ = 0;

    std::atomic<uint64_t> next_id_{1};
    mutable std::recursive_mutex mutex_;
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/sched/calendar_queue.hpp
git commit -m "feat: add CalendarQueue header — three-level calendar timer queue"
```

---

### Task 2: Implement BucketList and Timer Allocation

**Files:**
- Modify: `src/sched/calendar_queue.cpp` (create)

- [ ] **Step 1: Write BucketList::push_back**

```cpp
void CalendarQueue::BucketList::push_back(Timer* t) {
    t->next = nullptr;
    t->prev = tail;
    if (tail) {
        tail->next = t;
    } else {
        head = t;
    }
    tail = t;
    count++;
}
```

- [ ] **Step 2: Write BucketList::unlink**

```cpp
void CalendarQueue::BucketList::unlink(Timer* t) {
    if (t->prev) {
        t->prev->next = t->next;
    } else {
        head = t->next;
    }
    if (t->next) {
        t->next->prev = t->prev;
    } else {
        tail = t->prev;
    }
    t->next = nullptr;
    t->prev = nullptr;
    count--;
}
```

- [ ] **Step 3: Commit**

```bash
git add src/sched/calendar_queue.cpp
git commit -m "feat: add CalendarQueue BucketList — intrusive linked list ops"
```

---

### Task 3: Implement Constructor and Destructor

**Files:**
- Modify: `src/sched/calendar_queue.cpp`

- [ ] **Step 1: Write constructor with static_assert and initialization**

```cpp
CalendarQueue::CalendarQueue(const Config& cfg)
    : fine_bucket_ns_(cfg.fine_bucket_ns)
    , coarse_bucket_ns_(cfg.fine_bucket_ns * cfg.fine_buckets)
    , remote_bucket_ns_(coarse_bucket_ns_ * cfg.coarse_buckets)
    , max_advance_buckets_(cfg.max_advance_buckets)
{
    static_assert((256 & 255) == 0, "default bucket counts must be power of 2");

    if ((cfg.fine_buckets & (cfg.fine_buckets - 1)) != 0 ||
        (cfg.coarse_buckets & (cfg.coarse_buckets - 1)) != 0 ||
        (cfg.remote_buckets & (cfg.remote_buckets - 1)) != 0) {
        std::abort();  // bucket counts must be powers of 2
    }

    fine_mask_   = cfg.fine_buckets - 1;
    coarse_mask_ = cfg.coarse_buckets - 1;
    remote_mask_ = cfg.remote_buckets - 1;

    fine_wheel_.resize(cfg.fine_buckets);
    coarse_wheel_.resize(cfg.coarse_buckets);
    remote_wheel_.resize(cfg.remote_buckets);
}
```

- [ ] **Step 2: Write destructor**

```cpp
CalendarQueue::~CalendarQueue() {
    // Free all remaining Timer nodes. Callbacks are NOT fired.
    for (auto& [id, timer] : timer_map_) {
        delete timer;
    }
}
```

- [ ] **Step 3: Add includes to the .cpp file**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/sched/calendar_queue.hpp>
#include <algorithm>
#include <cstdlib>
```

- [ ] **Step 4: Commit**

```bash
git add src/sched/calendar_queue.cpp
git commit -m "feat: add CalendarQueue constructor/destructor with power-of-2 validation"
```

---

### Task 4: Implement schedule and insert_timer

**Files:**
- Modify: `src/sched/calendar_queue.cpp`

- [ ] **Step 1: Write schedule and schedule_at**

```cpp
uint64_t CalendarQueue::schedule(int64_t delay_ns, TimerCallback cb) {
    int64_t now = last_advance_ns_;  // use last known time (no lock needed for read)
    int64_t expire_ns = now + delay_ns;
    if (delay_ns <= 0) {
        expire_ns = now + fine_bucket_ns_;
    }
    return schedule_at(expire_ns, std::move(cb));
}

uint64_t CalendarQueue::schedule_at(int64_t expire_ns, TimerCallback cb) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto* timer = new Timer;
    timer->expire_ns = expire_ns;
    timer->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    timer->callback = std::move(cb);

    timer_map_[timer->id] = timer;

    int64_t now = last_advance_ns_;
    insert_timer(timer, now);
    return timer->id;
}
```

- [ ] **Step 2: Write insert_timer — the core placement logic**

```cpp
void CalendarQueue::insert_timer(Timer* timer, int64_t now_ns) {
    int64_t expire = std::max(timer->expire_ns, now_ns + fine_bucket_ns_);

    if (expire < now_ns + fine_bucket_ns_ * fine_mask_ + fine_bucket_ns_) {
        // Fine wheel: within ~256ms
        uint32_t bucket = static_cast<uint32_t>(expire / fine_bucket_ns_) & fine_mask_;
        timer->bucket_idx = bucket;
        timer->wheel_level = 0;
        fine_wheel_[bucket].push_back(timer);
    } else if (expire < now_ns + coarse_bucket_ns_ * coarse_mask_ + coarse_bucket_ns_) {
        // Coarse wheel: within ~65s
        uint32_t bucket = static_cast<uint32_t>(expire / coarse_bucket_ns_) & coarse_mask_;
        timer->bucket_idx = bucket;
        timer->wheel_level = 1;
        coarse_wheel_[bucket].push_back(timer);
    } else {
        // Remote wheel: >65s
        uint32_t bucket = static_cast<uint32_t>(expire / remote_bucket_ns_) & remote_mask_;
        timer->bucket_idx = bucket;
        timer->wheel_level = 2;
        remote_wheel_[bucket].push_back(timer);
    }
}
```

- [ ] **Step 3: Commit**

```bash
git add src/sched/calendar_queue.cpp
git commit -m "feat: add CalendarQueue schedule and insert_timer"
```

---

### Task 5: Implement cancel

**Files:**
- Modify: `src/sched/calendar_queue.cpp`

- [ ] **Step 1: Write cancel**

```cpp
bool CalendarQueue::cancel(uint64_t timer_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = timer_map_.find(timer_id);
    if (it == timer_map_.end()) {
        return false;
    }

    Timer* timer = it->second;
    timer_map_.erase(it);

    // Unlink from its bucket list
    switch (timer->wheel_level) {
    case 0: fine_wheel_[timer->bucket_idx].unlink(timer);   break;
    case 1: coarse_wheel_[timer->bucket_idx].unlink(timer); break;
    case 2: remote_wheel_[timer->bucket_idx].unlink(timer); break;
    }

    delete timer;
    return true;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/sched/calendar_queue.cpp
git commit -m "feat: add CalendarQueue cancel — O(1) id lookup + unlink"
```

---

### Task 6: Implement advance with cascade

**Files:**
- Modify: `src/sched/calendar_queue.cpp`

- [ ] **Step 1: Write cascade_coarse — move one coarse bucket into the fine wheel**

```cpp
void CalendarQueue::cascade_coarse(int64_t now_ns) {
    auto& bucket = coarse_wheel_[current_coarse_];
    Timer* t = bucket.head;
    while (t) {
        Timer* next = t->next;
        bucket.unlink(t);
        insert_timer(t, now_ns);
        t = next;
    }
}
```

- [ ] **Step 2: Write cascade_remote — move one remote bucket into the coarse wheel**

```cpp
void CalendarQueue::cascade_remote(int64_t now_ns) {
    auto& bucket = remote_wheel_[current_remote_];
    Timer* t = bucket.head;
    while (t) {
        Timer* next = t->next;
        bucket.unlink(t);
        insert_timer(t, now_ns);
        t = next;
    }
}
```

- [ ] **Step 3: Write advance — the main tick loop**

```cpp
uint32_t CalendarQueue::advance(int64_t now_ns) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (now_ns <= last_advance_ns_) {
        return 0;
    }

    // On first call, jump directly to now (no catch-up)
    if (last_advance_ns_ == 0) {
        last_advance_ns_ = now_ns;
        return 0;
    }

    uint32_t fired = 0;
    uint32_t buckets_processed = 0;

    while (last_advance_ns_ + fine_bucket_ns_ <= now_ns &&
           buckets_processed < max_advance_buckets_) {

        // 1. Fire expired timers in current fine bucket
        auto& bucket = fine_wheel_[current_fine_];
        Timer* t = bucket.head;
        while (t) {
            Timer* next = t->next;
            bucket.unlink(t);
            timer_map_.erase(t->id);

            if (t->expire_ns <= now_ns) {
                t->callback();   // lock held (recursive_mutex)
                fired++;
            }
            delete t;
            t = next;
        }

        // 2. Advance fine pointer
        last_advance_ns_ += fine_bucket_ns_;
        current_fine_ = (current_fine_ + 1) & fine_mask_;
        buckets_processed++;

        // 3. On fine wrap: cascade one coarse bucket
        if (current_fine_ == 0) {
            cascade_coarse(now_ns);
            current_coarse_ = (current_coarse_ + 1) & coarse_mask_;

            // 4. On coarse wrap: cascade one remote bucket
            if (current_coarse_ == 0) {
                cascade_remote(now_ns);
                current_remote_ = (current_remote_ + 1) & remote_mask_;
            }
        }
    }

    return fired;
}
```

- [ ] **Step 4: Write empty()**

```cpp
bool CalendarQueue::empty() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return timer_map_.empty();
}
```

- [ ] **Step 5: Commit**

```bash
git add src/sched/calendar_queue.cpp
git commit -m "feat: add CalendarQueue advance with three-level cascade"
```

---

### Task 7: Add CalendarQueue to HybridScheduler via variant

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `src/sched/scheduler.cpp`

- [ ] **Step 1: Add TimerBackend enum and variant to scheduler.hpp**

Read `include/hpactor/sched/scheduler.hpp`. After the `TimerHandle` definition (line 51), add:

```cpp
enum class TimerBackend : uint8_t {
    TimingWheel = 0,
    CalendarQueue = 1
};
```

Add `#include <hpactor/sched/calendar_queue.hpp>` to the includes.

Change the private section: replace `TimingWheel timer_wheel_;` with:

```cpp
std::variant<TimingWheel, CalendarQueue> timer_backend_;
```

Change `schedule_timer` declaration (line 168) from:

```cpp
uint64_t schedule_timer(int64_t delay_ns, TimingWheel::TimerCallback callback);
```

to:

```cpp
uint64_t schedule_timer(int64_t delay_ns, timer_callback callback);
```

- [ ] **Step 2: Update HybridScheduler constructor in scheduler.hpp**

Change constructor declaration to accept `TimerBackend`:

```cpp
explicit HybridScheduler(ActorSystem& system, uint32_t num_workers,
                         uint32_t num_priorities = 4,
                         TimerBackend timer_backend = TimerBackend::TimingWheel);
```

- [ ] **Step 3: Update scheduler.cpp — constructor with variant construction**

Read `src/sched/scheduler.cpp` lines 44-50. Change to:

```cpp
HybridScheduler::HybridScheduler(ActorSystem& system, uint32_t num_workers,
                                 uint32_t num_priorities,
                                 TimerBackend timer_backend)
    : system_(system), num_workers_(num_workers), num_priorities_(num_priorities),
      workers_(num_workers), a2ws_(num_workers),
      timer_backend_(std::in_place_type<TimingWheel>, 1'000'000, 4),
      dedicated_(std::make_unique<DedicatedStorage>()) {
    if (timer_backend == TimerBackend::CalendarQueue) {
        timer_backend_.emplace<CalendarQueue>();
    }
```

- [ ] **Step 4: Update scheduler.cpp — dispatch schedule_after/schedule_every/cancel_timer/advance_time via visit**

Replace body of `schedule_after` (line 371):

```cpp
TimerHandle HybridScheduler::schedule_after(timer_callback cb, int64_t delay_ns) {
    auto id = std::visit([&](auto& backend) {
        return backend.schedule(delay_ns, cb);
    }, timer_backend_);
    return TimerHandle{id};
}
```

Replace body of `schedule_every` (line 377): change `timer_wheel_.schedule(*interval, recurring)` calls (lines 393 and 398) to:

```cpp
std::visit([&](auto& backend) {
    backend.schedule(*interval, recurring);
}, timer_backend_);
```

Replace body of `cancel_timer` (line 406): change `timer_wheel_.cancel(handle.id)` to:

```cpp
std::visit([&](auto& backend) {
    backend.cancel(handle.id);
}, timer_backend_);
```

Replace body of `advance_time` (line 367):

```cpp
void HybridScheduler::advance_time(int64_t now_ns) {
    std::visit([&](auto& backend) {
        backend.advance(now_ns);
    }, timer_backend_);
}
```

Replace body of `schedule_timer` (line 362):

```cpp
uint64_t HybridScheduler::schedule_timer(int64_t delay_ns,
                                          timer_callback callback) {
    return std::visit([&](auto& backend) {
        return backend.schedule(delay_ns, std::move(callback));
    }, timer_backend_);
}
```

- [ ] **Step 5: Add include to scheduler.cpp**

Add at top of `src/sched/scheduler.cpp`:

```cpp
#include <variant>
```

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp
git commit -m "feat: add TimerBackend enum and variant dispatch to HybridScheduler"
```

---

### Task 8: Update ActorSystem Config

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`

- [ ] **Step 1: Add timer_backend to Config**

Read `include/hpactor/core/actor_system.hpp`. Find the `Config` struct (line 67). After the CLI config line (line 102), add:

```cpp
// Timer backend selection
sched::TimerBackend timer_backend = sched::TimerBackend::TimingWheel;
```

- [ ] **Step 2: Add include for TimerBackend**

The scheduler header is already included. `TimerBackend` is in `scheduler.hpp`. Verify it's accessible from `actor_system.hpp` (the scheduler header is already included transitively or directly — check and add if needed).

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/core/actor_system.hpp
git commit -m "feat: add timer_backend to ActorSystem Config"
```

---

### Task 9: Update coroutine_awaiters.hpp for schedule_timer type change

**Files:**
- Modify: `include/hpactor/sched/coroutine_awaiters.hpp`

- [ ] **Step 1: Check and update TimerAwaiter if needed**

Read `include/hpactor/sched/coroutine_awaiters.hpp` around line 112. The `schedule_timer` call now accepts `sched::timer_callback` instead of `TimingWheel::TimerCallback`. Since both are `std::function<void()>`, the lambda `[this] { scheduler_.notify_ready(...); }` is already compatible. No code change needed — verify this compiles.

- [ ] **Step 2: Commit (or skip if no changes)**

```bash
# Only if changes needed:
git add include/hpactor/sched/coroutine_awaiters.hpp
git commit -m "fix: update schedule_timer callback type reference"
```

---

### Task 10: Add test target to CMakeLists

**Files:**
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add test_calendar_queue target**

After the last scheduler test entry in `tests/CMakeLists.txt`, add:

```cmake
add_executable(test_calendar_queue sched/test_calendar_queue.cpp)
target_link_libraries(test_calendar_queue hpactor)
add_test(NAME test_calendar_queue COMMAND test_calendar_queue)
```

- [ ] **Step 2: Run cmake reconfigure and verify test is registered**

```bash
cmake -S . -B build -GNinja
ninja -C build test_calendar_queue
./build/tests/test_calendar_queue
```

Expected: "test_calendar_queue: PASSED" (with empty test file — no tests yet)

- [ ] **Step 3: Commit**

```bash
git add tests/CMakeLists.txt tests/sched/test_calendar_queue.cpp
git commit -m "test: add test_calendar_queue target"
```

---

### Task 11: Write unit tests — basic schedule and cancel

**Files:**
- Modify: `tests/sched/test_calendar_queue.cpp`

- [ ] **Step 1: Write test_calendar_basic_schedule**

```cpp
#include <hpactor/sched/calendar_queue.hpp>
#include <cassert>
#include <cstdio>

using namespace hpactor::sched;

static int fired_count = 0;
static void reset_count() { fired_count = 0; }

void test_calendar_basic_schedule() {
    CalendarQueue cq;
    reset_count();

    cq.schedule(1'000'000, [] { fired_count++; });   // 1ms delay
    assert(cq.size() == 1);

    // Advance by 2ms — timer should fire
    int64_t now = 2'000'000;
    uint32_t fired = cq.advance(now);
    assert(fired == 1);
    assert(fired_count == 1);
    assert(cq.size() == 0);
    assert(cq.empty());
}

int main() {
    test_calendar_basic_schedule();
    printf("test_calendar_queue: PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build test_calendar_queue && ./build/tests/test_calendar_queue
```

Expected: `test_calendar_queue: PASSED`

- [ ] **Step 3: Write test_calendar_cancel**

```cpp
void test_calendar_cancel() {
    CalendarQueue cq;
    reset_count();

    auto id = cq.schedule(1'000'000, [] { fired_count++; });
    assert(cq.size() == 1);

    bool ok = cq.cancel(id);
    assert(ok);
    assert(cq.size() == 0);

    // Advance — timer should NOT fire
    uint32_t fired = cq.advance(2'000'000);
    assert(fired == 0);
    assert(fired_count == 0);
}
```

- [ ] **Step 4: Write test_calendar_cancel_nonexistent**

```cpp
void test_calendar_cancel_nonexistent() {
    CalendarQueue cq;
    bool ok = cq.cancel(999);  // never scheduled
    assert(!ok);
}
```

- [ ] **Step 5: Write test_calendar_id_valid**

```cpp
void test_calendar_id_valid() {
    CalendarQueue cq;
    auto id = cq.schedule(1'000'000, [] {});
    assert(id >= 1);
    // TimerHandle(id=0).valid() should be false — matches scheduler contract
    TimerHandle h0{0};
    assert(!h0.valid());
    TimerHandle h1{id};
    assert(h1.valid());
}
```

- [ ] **Step 6: Write test_calendar_zero_delay_clamped**

```cpp
void test_calendar_zero_delay_clamped() {
    CalendarQueue cq;
    reset_count();

    cq.schedule(0, [] { fired_count++; });   // 0 delay -> clamped to 1 fine bucket
    assert(cq.size() == 1);

    // Advance by 1ms — should fire
    uint32_t fired = cq.advance(2'000'000);
    assert(fired == 1);
    assert(fired_count == 1);
}
```

- [ ] **Step 7: Update main() to call all tests**

```cpp
int main() {
    test_calendar_basic_schedule();
    test_calendar_cancel();
    test_calendar_cancel_nonexistent();
    test_calendar_id_valid();
    test_calendar_zero_delay_clamped();
    reset_count();
    printf("test_calendar_queue: PASSED\n");
    return 0;
}
```

- [ ] **Step 8: Build, run, commit**

```bash
ninja -C build test_calendar_queue && ./build/tests/test_calendar_queue
git add tests/sched/test_calendar_queue.cpp
git commit -m "test: add CalendarQueue basic schedule, cancel, and edge case tests"
```

---

### Task 12: Write unit tests — multi-level and cascade

**Files:**
- Modify: `tests/sched/test_calendar_queue.cpp`

- [ ] **Step 1: Write test_calendar_fine_coarse_split**

Schedule at 300ms (beyond 256ms fine range → coarse wheel). Advance in 1ms increments, verify it fires at the right time.

```cpp
void test_calendar_fine_coarse_split() {
    CalendarQueue cq;
    reset_count();

    // 300ms — goes to coarse wheel
    cq.schedule(300'000'000, [] { fired_count++; });
    assert(cq.size() == 1);

    // Advance 300ms + 1ms in 1ms steps (simulates real timer thread)
    int64_t now = 0;
    uint32_t total_fired = 0;
    for (int i = 0; i < 302; i++) {
        now += 1'000'000;
        total_fired += cq.advance(now);
    }
    assert(total_fired == 1);
    assert(fired_count == 1);
    assert(cq.empty());
}
```

- [ ] **Step 2: Write test_calendar_cascade_coarse**

Place 5 timers in a single coarse bucket, verify they cascade into the fine wheel correctly.

```cpp
void test_calendar_cascade_coarse() {
    CalendarQueue cq;
    reset_count();
    int count = 0;

    // All 5 go to the same coarse bucket (~300ms from now, at t=0)
    for (int i = 0; i < 5; i++) {
        cq.schedule(300'000'000, [&count] { count++; });
    }
    assert(cq.size() == 5);

    // Advance through 300ms in 1ms steps
    int64_t now = 0;
    uint32_t total_fired = 0;
    for (int i = 0; i < 302; i++) {
        now += 1'000'000;
        total_fired += cq.advance(now);
    }
    assert(total_fired == 5);
    assert(count == 5);
    assert(cq.empty());
}
```

- [ ] **Step 3: Write test_calendar_remote_cascade**

Timer at 70s (beyond coarse 65s range → remote wheel).

```cpp
void test_calendar_remote_cascade() {
    CalendarQueue cq;
    reset_count();

    // 70 seconds — goes to remote wheel
    cq.schedule(70'000'000'000LL, [] { fired_count++; });
    assert(cq.size() == 1);

    // Advance 70 seconds + 1ms in 1ms steps
    int64_t now = 0;
    uint32_t total_fired = 0;
    for (int64_t i = 0; i < 70001; i++) {
        now += 1'000'000;
        total_fired += cq.advance(now);
    }
    assert(total_fired == 1);
    assert(fired_count == 1);
    assert(cq.empty());
}
```

- [ ] **Step 4: Build, run, commit**

```bash
ninja -C build test_calendar_queue && ./build/tests/test_calendar_queue
git add tests/sched/test_calendar_queue.cpp
git commit -m "test: add CalendarQueue multi-level cascade tests"
```

---

### Task 13: Write unit tests — re-entrant safety and time jumps

**Files:**
- Modify: `tests/sched/test_calendar_queue.cpp`

- [ ] **Step 1: Write test_calendar_recurring_no_deadlock**

A callback that calls `schedule()` (simulates `schedule_every` wrapper). This exercises the `recursive_mutex`.

```cpp
void test_calendar_recurring_no_deadlock() {
    CalendarQueue cq;
    int count = 0;

    std::function<void()> recurring;
    recurring = [&] {
        count++;
        if (count < 5) {
            cq.schedule(1'000'000, recurring);  // self-reschedule during advance
        }
    };

    cq.schedule(1'000'000, recurring);

    int64_t now = 0;
    for (int i = 0; i < 10; i++) {
        now += 2'000'000;  // 2ms per step
        cq.advance(now);
    }
    assert(count == 5);
    assert(cq.empty());
}
```

- [ ] **Step 2: Write test_calendar_cancel_during_advance**

Callback cancels another timer during `advance()`.

```cpp
void test_calendar_cancel_during_advance() {
    CalendarQueue cq;
    int count_a = 0, count_b = 0;

    uint64_t id_b = 0;
    cq.schedule(2'000'000, [&] {
        count_a++;
        cq.cancel(id_b);  // cancel B during A's callback
    });
    id_b = cq.schedule(2'000'000, [&] { count_b++; });

    uint32_t fired = cq.advance(3'000'000);
    // A fired, B was cancelled during A's callback — fired count only counts A
    assert(count_a == 1);
    assert(count_b == 0);
    assert(cq.empty());
}
```

- [ ] **Step 3: Write test_calendar_time_jump**

Advance by 5ms in one call — all intermediate timers should fire.

```cpp
void test_calendar_time_jump() {
    CalendarQueue cq;
    int count = 0;

    // Schedule at 1ms, 2ms, 3ms
    cq.schedule(1'000'000, [&] { count++; });
    cq.schedule(2'000'000, [&] { count++; });
    cq.schedule(3'000'000, [&] { count++; });

    // Jump ahead 5ms in one advance call
    uint32_t fired = cq.advance(5'000'000);
    assert(fired == 3);
    assert(count == 3);
    assert(cq.empty());
}
```

- [ ] **Step 4: Write test_calendar_time_backwards**

```cpp
void test_calendar_time_backwards() {
    CalendarQueue cq;
    cq.schedule(10'000'000, [] {});

    cq.advance(5'000'000);   // set last_advance to 5ms
    uint32_t fired = cq.advance(3'000'000);  // go backwards
    assert(fired == 0);  // no-op
}
```

- [ ] **Step 5: Write test_calendar_many_timers**

```cpp
void test_calendar_many_timers() {
    CalendarQueue cq;
    int count = 0;

    for (int i = 0; i < 10000; i++) {
        cq.schedule(1'000'000 + (i % 100) * 1000, [&] { count++; });
    }
    assert(cq.size() == 10000);

    uint32_t fired = cq.advance(2'000'000);
    assert(fired > 0);
    assert(static_cast<size_t>(fired) + cq.size() == 10000);
}
```

- [ ] **Step 6: Write test_calendar_empty**

```cpp
void test_calendar_empty() {
    CalendarQueue cq;
    assert(cq.empty());
    assert(cq.size() == 0);

    auto id = cq.schedule(1'000'000, [] {});
    assert(!cq.empty());
    assert(cq.size() == 1);

    cq.cancel(id);
    assert(cq.empty());
    assert(cq.size() == 0);

    // Advance on empty queue — no-op
    uint32_t fired = cq.advance(10'000'000);
    assert(fired == 0);
}
```

- [ ] **Step 7: Update main() to call all new tests, build, run, commit**

```bash
ninja -C build test_calendar_queue && ./build/tests/test_calendar_queue
git add tests/sched/test_calendar_queue.cpp
git commit -m "test: add CalendarQueue re-entrant safety, time jump, and stress tests"
```

---

### Task 14: Run full test suite and verify

**Files:**
- None (verification only)

- [ ] **Step 1: Reconfigure and build everything**

```bash
cmake -S . -B build -GNinja
ninja -C build
```

Expected: clean build, no errors.

- [ ] **Step 2: Run all existing tests**

```bash
ctest --output-on-failure
```

Expected: all existing 95 tests pass + new test_calendar_queue (15 test cases) = 96 tests.

- [ ] **Step 3: Run with CalendarQueue backend**

Create a quick smoke test or modify an example to use `TimerBackend::CalendarQueue`:

```bash
# In an example or test, verify calendar queue backend works
# Config:
#   Config config{.scheduler_threads = 1, .max_queue_depth = 1024, .cli = {}};
#   // No API yet — verify via programmatic test
```

- [ ] **Step 4: Verify test passes**

Expected: `100% tests passed, 0 tests failed out of 96`

- [ ] **Step 5: Commit any remaining changes**

```bash
git add -A
git commit -m "chore: finalize CalendarQueue implementation — all tests passing"
```

---

## Test Plan Summary

| Test | Task | Validates |
|------|------|-----------|
| `test_calendar_basic_schedule` | 11 | Schedule one-shot, advance fires it |
| `test_calendar_cancel` | 11 | Cancel prevents firing |
| `test_calendar_cancel_nonexistent` | 11 | Cancel invalid id returns false |
| `test_calendar_id_valid` | 11 | Timer ID >= 1, Handle(id=0).valid() is false |
| `test_calendar_zero_delay_clamped` | 11 | Zero delay clamped to 1 fine bucket |
| `test_calendar_fine_coarse_split` | 12 | 300ms timer goes to coarse, cascades to fine |
| `test_calendar_cascade_coarse` | 12 | Multiple timers in one coarse bucket cascade correctly |
| `test_calendar_remote_cascade` | 12 | 70s timer in remote wheel cascades through coarse to fine |
| `test_calendar_recurring_no_deadlock` | 13 | Self-rescheduling callback during advance (recursive_mutex) |
| `test_calendar_cancel_during_advance` | 13 | Callback cancels another timer during advance |
| `test_calendar_time_jump` | 13 | Advance by 5ms fires all intermediate timers |
| `test_calendar_time_backwards` | 13 | Advance with now < last is no-op |
| `test_calendar_many_timers` | 13 | 10k timers, fire some, verify count |
| `test_calendar_empty` | 13 | empty() and size() after insert/cancel/fire |

**Total: 14 test cases in 1 test suite**
