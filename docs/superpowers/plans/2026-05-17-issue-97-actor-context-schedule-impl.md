# ActorContext::schedule() Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the currently stubbed `ActorContext::schedule()` and add `cancel_schedule()` so actors can schedule self-delivered messages after a delay, using the existing `IScheduler::schedule_after()` / `TimingWheel` infrastructure.

**Architecture:** `schedule(delay, msg)` sets the message's sender to the calling actor's own address, wraps a `system_->deliver_local(owner_id, msg)` call in a `std::function<void()>`, and passes it to `scheduler->schedule_after(callback, delay_ns)`. Returns `AlarmHandle` so the caller can cancel via `cancel_schedule()`. The callback fires on the timer thread; `deliver_local()` is lock-free and cross-thread safe.

**Tech Stack:** C++20, existing `IScheduler`/`TimingWheel`/`CalendarQueue`, existing `MPSCActorMailbox`, existing `AlarmHandle` type.

**Spec:** `docs/superpowers/specs/2026-05-17-issue-97-actor-context-schedule-design.md`

---

## File Structure

| File | Purpose |
|------|---------|
| `include/hpactor/actor_context.hpp` | **Modified** — change `schedule()` return type `void` → `AlarmHandle`; add `cancel_schedule(AlarmHandle)` declaration |
| `src/actor/actor_context.cpp` | **Modified** — implement `schedule()` and `cancel_schedule()` |
| `tests/actor/test_actor_context_schedule.cpp` | **Create** — unit tests for schedule/cancel |
| `tests/CMakeLists.txt` | **Modified** — add `test_actor_context_schedule` |
| `examples/13_order_platform.cpp` | **Modified** — payment-timeout `context()->schedule()` call now uses the implemented path (no source change needed, just starts working) |

---

### Task 1: Change API Signatures in actor_context.hpp

**File:** `include/hpactor/actor_context.hpp`

- [ ] **Step 1: Update `schedule()` declaration**

Change the return type from `void` to `AlarmHandle`:

```cpp
// Before:
void schedule(std::chrono::milliseconds delay, TypedMessage msg);

// After:
AlarmHandle schedule(std::chrono::milliseconds delay, TypedMessage msg);
```

- [ ] **Step 2: Add `cancel_schedule()` declaration**

Add after `schedule()`:

```cpp
void cancel_schedule(AlarmHandle handle);
```

Include `<hpactor/types/types.hpp>` if not already present (for `AlarmHandle`).

---

### Task 2: Implement `schedule()` in actor_context.cpp

**File:** `src/actor/actor_context.cpp`

- [ ] **Step 1: Replace the stub**

The current stub at ~line 213:

```cpp
void ActorContext::schedule(std::chrono::milliseconds delay, TypedMessage msg) {
    // TODO: schedule message via actor system's clock/alarm mechanism
    (void)delay;
    (void)msg;
}
```

Replace with:

```cpp
AlarmHandle ActorContext::schedule(std::chrono::milliseconds delay,
                                   TypedMessage msg) {
    auto* sched = system_->scheduler();
    if (!sched) return AlarmHandle{};

    msg.set_sender_address(owner_->address());

    ActorId self_id = owner_->id();
    ActorSystem* sys = system_;

    auto callback = [sys, self_id, msg = std::move(msg)]() mutable {
        sys->deliver_local(self_id, std::move(msg));
    };

    int64_t delay_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
    auto handle = sched->schedule_after(std::move(callback), delay_ns);
    return AlarmHandle{handle.id};
}
```

- Includes needed: `<hpactor/core/actor_system.hpp>`, `<hpactor/actor/abstract_actor.hpp>`, `<hpactor/sched/scheduler.hpp>`

---

### Task 3: Implement `cancel_schedule()` in actor_context.cpp

**File:** `src/actor/actor_context.cpp`

- [ ] **Step 1: Add implementation**

```cpp
void ActorContext::cancel_schedule(AlarmHandle handle) {
    if (!handle.valid()) return;
    auto* sched = system_->scheduler();
    if (!sched) return;
    sched->cancel_timer(sched::TimerHandle{handle.id()});
}
```

---

### Task 4: Write Unit Tests

**File:** Create `tests/actor/test_actor_context_schedule.cpp`

- [ ] **Step 1: Test actor that records scheduled message arrival**

```cpp
// A test actor that schedules a self-message and records when it arrives.
class ScheduleTestActor : public EventBasedActor {
public:
    ScheduleTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    bool message_received() const { return received_.load(); }
    int64_t elapsed_ms() const { return elapsed_ms_; }

    void trigger_schedule(std::chrono::milliseconds delay) {
        auto start = std::chrono::steady_clock::now();
        start_time_ = start.time_since_epoch().count();
        context()->schedule(delay, TypedMessage(UserTag,
            StreamBuffer{42}));  // arbitrary payload
    }

    void trigger_schedule_and_cancel(std::chrono::milliseconds delay) {
        auto handle = context()->schedule(delay, TypedMessage(UserTag,
            StreamBuffer{42}));
        context()->cancel_schedule(handle);
    }

    bool cancel_before_schedule_returns_invalid() {
        auto handle = context()->schedule(std::chrono::milliseconds(10),
                                          TypedMessage(UserTag, StreamBuffer{}));
        context()->cancel_schedule(handle);
        // Second cancel on same handle should be harmless.
        context()->cancel_schedule(handle);
        context()->cancel_schedule(AlarmHandle{}); // invalid handle no-op
        return true;  // no crash = pass
    }

protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == UserTag) {
                auto now = std::chrono::steady_clock::now()
                    .time_since_epoch().count();
                elapsed_ms_ = (now - start_time_) / 1'000'000;
                received_.store(true);
            }
        }};
    }

private:
    std::atomic<bool> received_{false};
    int64_t start_time_ = 0;
    int64_t elapsed_ms_ = 0;
};
```

- [ ] **Step 2: test_schedule_delivers_after_delay**

```cpp
static void test_schedule_delivers_after_delay() {
    Config config;
    config.scheduler_threads = 2;  // needs timer thread running
    ActorSystem system(config);

    auto actor = system.spawn<ScheduleTestActor>();
    actor->trigger_schedule(std::chrono::milliseconds(50));

    // Poll with generous timeout.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!actor->message_received() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    assert(actor->message_received());
    auto elapsed = actor->elapsed_ms();
    // Allow 10ms undershoot, generous overshoot for CI.
    assert(elapsed >= 40);
    assert(elapsed < 5000);
}
```

- [ ] **Step 3: test_cancel_schedule_prevents_delivery**

```cpp
static void test_cancel_schedule_prevents_delivery() {
    Config config;
    config.scheduler_threads = 2;
    ActorSystem system(config);

    auto actor = system.spawn<ScheduleTestActor>();
    actor->trigger_schedule_and_cancel(std::chrono::milliseconds(200));

    // Wait longer than the scheduled delay.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    assert(!actor->message_received());
}
```

- [ ] **Step 4: test_cancel_schedule_no_crash_on_double_cancel**

```cpp
static void test_cancel_schedule_no_crash_on_double_cancel() {
    Config config;
    config.scheduler_threads = 2;
    ActorSystem system(config);

    auto actor = system.spawn<ScheduleTestActor>();
    assert(actor->cancel_before_schedule_returns_invalid());
}
```

- [ ] **Step 5: main() entry point**

```cpp
int main() {
    test_schedule_delivers_after_delay();
    test_cancel_schedule_prevents_delivery();
    test_cancel_schedule_no_crash_on_double_cancel();
    return 0;
}
```

---

### Task 5: Register Test in CMakeLists.txt

**File:** `tests/CMakeLists.txt`

- [ ] **Step 1: Add test target**

```cmake
add_hpactor_test(test_actor_context_schedule
    SOURCES actor/test_actor_context_schedule.cpp)
```

---

### Task 6: Build and Verify

- [ ] **Step 1: Build the framework and test**

```bash
cmake -S . -B build -GNinja
ninja -C build test_actor_context_schedule
```

- [ ] **Step 2: Run new tests**

```bash
./build/tests/test_actor_context_schedule
```

Expected: all 3 assertions pass.

- [ ] **Step 3: Run payment-timeout scenario**

```bash
./build/examples/13_order_platform --all-in-one --scenario payment-timeout
```

Expected: `status=payment_timed_out` (was previously silently dropped).

- [ ] **Step 4: Run full test suite**

```bash
ctest --output-on-failure --parallel 8
```

Expected: 141 tests pass (140 existing + 1 new).
