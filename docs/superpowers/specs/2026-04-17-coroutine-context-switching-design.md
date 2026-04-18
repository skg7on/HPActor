# Architecture Design: Coroutine Context Switching

## Executive Summary

This document specifies the **missing coroutine context-switching pipeline** — the mechanism that connects `HybridScheduler::execute_actor()` to actual `coroutine_handle::resume()` calls, wires `MPSCMailbox` enqueue to continuation wakeup via edge-trigger, and provides a `yield()` primitive for cooperative multitasking. This is the glue between the already-implemented types (`CoroutineTask`, `ActorState`, `MailboxAwaiter`) and the scheduler.

**Status of existing infrastructure:**

| Component | Location | Status |
|-----------|----------|--------|
| `CoroutineTask` | `sched/coroutine_task.hpp` | ✅ Implemented |
| `CoroutinePromise` | `sched/coroutine_task.hpp` | ✅ Implemented |
| `ActorState` atomic state | `actor/actor_state.hpp` | ✅ Implemented |
| `MailboxAwaiter` / `TimerAwaiter` | `sched/coroutine_awaiters.hpp` | ✅ Types exist, unwired |
| `MPSCMailbox<T>` | `mailbox/mpsc_mailbox.hpp` | ✅ Implemented |
| `HybridScheduler` | `sched/scheduler.hpp/cpp` | ✅ Infrastructure done |
| `execute_actor()` | `sched/scheduler.cpp:173` | ❌ **Stub — no resume()** |
| Actor `act()` entry point | `EventBasedActor` | ❌ **Doesn't return CoroutineTask** |
| `continuation.resume()` wakeup | MailboxAwaiter | ❌ **Never called** |
| `yield()` primitive | — | ❌ **Missing** |

---

## 1. Design Overview

### 1.1 The Missing Path

```
Message arrives at actor's MPSCMailbox
        │
        ▼
MPSCMailbox::enqueue() — sets actor to Ready via CAS
        │
        ▼
HybridScheduler::execute_actor(WorkItem)
        │
        ▼
CoroutinePromise::set_running() — Ready → Running
        │
        ▼
coroutine_handle::resume() — switch to actor stack
        │
        ▼
Actor code runs until co_await or co_return
        │
        ├─ co_await mailbox.receive()
        │       └─ MailboxAwaiter::await_suspend()
        │               ├─ Running → Idle (CAS)
        │               └─ stores continuation
        │               └─ returns false (don't suspend) if message already queued
        │
        ├─ co_await schedule_after(delay)
        │       └─ TimerAwaiter::await_suspend()
        │               └─ registers timer + continuation
        │
        ├─ co_await yield()
        │       └─ YieldAwaiter::await_suspend()
        │               ├─ Running → Ready (re-enqueue)
        │               └─ schedules immediate notify_ready
        │
        └─ co_return
                └─ CoroutinePromise::return_void()
                        └─ state → Terminated
```

### 1.2 Core Design Principles

1. **No blocking in workers** — all I/O and waits are non-blocking via awaiters
2. **Edge-trigger mailbox wakeup** — CAS on `mailbox_was_empty` (true→false) triggers wakeup
3. **Continuation-passing** — each suspend stores `continuation` handle; wakeup calls `continuation.resume()`
4. **State machine invariants** — `Running` can only transition to `Idle` or `IOWaiting` via CAS
5. **No exceptions/RTTI** — `std::error_code` for errors

---

## 2. Actor Entry Point: `act()` Coroutine

### 2.1 `ActorCoroutine` Class

Actors that use coroutines must define an `act()` method returning `CoroutineTask`. Since C++20 coroutines require the promise type to be associated with the return type, we define a concrete `ActorCoroutine` class that wraps the coroutine handle and owns the actor reference.

```cpp
// include/hpactor/sched/actor_coroutine.hpp
#pragma once

#include <hpactor/sched/coroutine_task.hpp>
#include <hpactor/actor/actor_state.hpp>

namespace hpactor::sched {

// ActorCoroutine: owns a coroutine handle for an actor.
// Produced by Actor::act() and consumed by HybridScheduler.
class ActorCoroutine {
public:
    ActorCoroutine() = default;
    explicit ActorCoroutine(CoroutineTask&& task, ActorId actor_id) noexcept
        : task_(std::move(task)), actor_id_(actor_id) {}

    // Move-only
    ActorCoroutine(ActorCoroutine&&) noexcept = default;
    ActorCoroutine& operator=(ActorCoroutine&&) noexcept = default;
    ActorCoroutine(const ActorCoroutine&) = delete;
    ActorCoroutine& operator=(const ActorCoroutine&) = delete;

    ~ActorCoroutine() {
        if (task_) task_.destroy();
    }

    explicit operator bool() const noexcept { return static_cast<bool>(task_); }

    CoroutineTask& task() { return task_; }
    const CoroutineTask& task() const { return task_; }

    ActorId actor_id() const { return actor_id_; }

    // Resume the coroutine. Must be called on the owning worker thread.
    void resume() {
        if (task_ && !task_.done()) {
            task_.resume();
        }
    }

    bool done() const { return !task_ || task_.done(); }

private:
    CoroutineTask task_;
    ActorId actor_id_;
};

} // namespace hpactor::sched
```

### 2.2 `EventBasedActor::act()` Pattern

```cpp
// include/hpactor/actor/event_based_actor.hpp (modified)
class EventBasedActor : public LocalActor {
public:
    // Override act() to return a coroutine — the actor's "main"
    virtual CoroutineTask act() { co_return; }

    // Called by scheduler to get this actor's coroutine (lazily created)
    ActorCoroutine& get_actor_coroutine() { return coroutine_; }

    // Called when actor first becomes ready
    void ensure_coroutine_started();

    // Mailbox state for awaiter
    bool mailbox_has_messages() const;
    bool mailbox_is_empty() const;

    void receive(MessageVariant&& msg) override;
    void become(Behavior bh);
    void become_empty();

protected:
    virtual Behavior make_behavior() { return {}; }
    void on_activate() override;
    void on_deactivate() override;
    virtual void on_exit() {}

    EventBasedActor(ActorContext* ctx, ActorSystem& sys);

private:
    ActorCoroutine coroutine_;
    Behavior behavior_;
};
```

```cpp
// src/actor/event_based_actor.cpp
void EventBasedActor::ensure_coroutine_started() {
    if (!coroutine_) {
        // Create the coroutine. The act() override is the coroutine body.
        // The coroutine starts suspended (initial_suspend), so it won't
        // run until the scheduler resumes it.
        auto task = act();
        coroutine_ = ActorCoroutine(std::move(task), id());
    }
}
```

### 2.3 Example Actor Using Coroutines

```cpp
class PingActor : public hpactor::EventBasedActor {
public:
    PingActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    hpactor::sched::CoroutineTask act() {
        while (true) {
            // co_await blocks until a message arrives
            auto msg = co_await context()->receive();

            // Handle messages via std::visit
            std::visit(hpactor::overloaded{
                [&](const Ping& ping) {
                    spdlog::info("PingActor received Ping");
                    context()->send(ping.from, Pong{});
                },
                [&](const Stop&) {
                    spdlog::info("PingActor stopping");
                    co_return;
                }
            }, msg);
        }
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::behavior_builder()
            .template on<Ping>([](Ping&& msg) {
                // Fallback behavior — used when not in coroutine mode
            });
    }
};
```

---

## 3. Mailbox Edge-Trigger Wakeup

### 3.1 The Problem

When a message arrives at an idle actor's mailbox, the actor must be woken up. The `MPSCMailbox` has no built-in wakeup mechanism — it just queues nodes. We need to wire `MPSCMailbox::enqueue()` to the actor's continuation resume.

### 3.2 `ActorMailbox` Wrapper

Wrap `MPSCMailbox` with a per-actor integration that tracks `mailbox_was_empty` and calls `scheduler_->notify_ready()` on transition to non-empty:

```cpp
// include/hpactor/mailbox/actor_mailbox.hpp
#pragma once

#include <hpactor/mailbox/mpsc_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <atomic>

namespace hpactor::mailbox {

// ActorMailbox: MPSCMailbox with edge-trigger scheduler wakeup.
// Embedded in each actor. When a message is enqueued to an empty mailbox,
// the first such enqueue triggers notify_ready to wake the actor.
template<typename T>
class ActorMailbox {
public:
    ActorMailbox(ActorId actor_id, sched::IScheduler* scheduler) noexcept
        : actor_id_(actor_id), scheduler_(scheduler) {}

    // Producer side: enqueue message and potentially wake actor
    void enqueue(T* node) noexcept {
        // Check if currently empty (before enqueue)
        bool was_empty = empty();

        // Actually enqueue
        mailbox_.enqueue(node);

        // Edge-trigger: if was empty, this is the transition empty→non-empty
        // Only the *first* enqueue after empty triggers notify_ready.
        // Subsequent enqueues (while non-empty) don't re-wake.
        if (was_empty) {
            // CAS mailbox_was_empty: true → false
            bool expected = true;
            if (mailbox_was_empty_.compare_exchange_strong(expected, false,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                // Successfully claimed the wakeup — schedule the actor
                scheduler_->notify_ready(actor_id_, 0, INT64_MAX);
            }
            // If CAS failed, another enqueue already claimed and scheduled
        }
    }

    // Consumer side: dequeue a message
    T* dequeue() noexcept {
        T* node = mailbox_.dequeue();
        // If we drained the mailbox, reset the edge-trigger flag
        if (node != nullptr && empty()) {
            mailbox_was_empty_.store(true, std::memory_order_release);
        }
        return node;
    }

    bool empty() const noexcept { return mailbox_.empty(); }

    // For awaiters to check mailbox state
    bool was_empty() const noexcept {
        return mailbox_was_empty_.load(std::memory_order_acquire);
    }

    void set_was_empty(bool val) noexcept {
        mailbox_was_empty_.store(val, std::memory_order_release);
    }

private:
    ActorId actor_id_;
    sched::IScheduler* scheduler_;
    MPSCMailbox<T> mailbox_;
    std::atomic<bool> mailbox_was_empty_{true};
};

} // namespace hpactor::mailbox
```

### 3.3 `ActorMailboxNode` — Intrusive Node for MPSCMailbox

The `MPSCMailbox<T>` requires `T` to have `std::atomic<T*> mpsc_next`. Each `MessageNode` in the actor system already has this. But for the edge-trigger, we need the `ActorMailbox` wrapper to access it.

```cpp
// In MessageNode (or wherever it's defined), ensure it has the intrusive link:
// (Assuming MessageNode already has this — verify with grep)
```

### 3.4 Integration with `EventBasedActor`

`EventBasedActor` holds an `ActorMailbox<MessageNode>` and exposes `mailbox_is_empty()` via it:

```cpp
// include/hpactor/actor/event_based_actor.hpp (modifications)
#include <hpactor/mailbox/actor_mailbox.hpp>

class EventBasedActor : public LocalActor {
    // ...
private:
    ActorMailbox<MessageNode> mailbox_{id(), nullptr};  // scheduler set on registration
};
```

The `mailbox_` pointer is set when the actor is registered with the system.

---

## 4. `execute_actor()` — Scheduler Integration

### 4.1 Revised `execute_actor()`

```cpp
// src/sched/scheduler.cpp
void HybridScheduler::execute_actor(const WorkItem& item) {
    auto* actor = system_.get_actor(item.actor);
    if (!actor) return;

    // Lazily start the coroutine on first pickup
    actor->ensure_coroutine_started();

    auto& coroutine = actor->get_actor_coroutine();
    if (!coroutine) return;

    auto& promise = coroutine.task().handle().promise();

    // Transition: Ready → Running
    // If already Running/Terminated, skip
    uint32_t expected = ActorState::kReady;
    if (!promise.state.cas(expected, ActorState::kRunning)) {
        if (promise.state.is_terminated()) {
            // Actor finished — clean up
            actor->on_exit();
            return;
        }
        // Actor already running or in IOWaiting — skip duplicate pickup
        return;
    }

    // Set owning worker
    promise.owner = current_worker();

    // Resume the coroutine
    coroutine.resume();

    // Post-resume check: coroutine suspended or terminated
    if (promise.is_idle()) {
        // Mailbox await — actor will be re-woken by mailbox edge-trigger
        // Nothing to do here; the mailbox will call notify_ready when non-empty
    } else if (promise.is_io_waiting()) {
        // I/O await — already registered with EventLoop
        // Nothing to do here
    } else if (promise.is_terminated()) {
        // Coroutine finished via co_return
        actor->on_exit();
    }
}
```

### 4.2 Revised `worker_loop()`

```cpp
void HybridScheduler::worker_loop(uint32_t worker_id) {
    // Set thread-local worker pointer for promise.owner
    set_current_worker(&workers_[worker_id]);

    while (running_.load(std::memory_order_acquire)) {
        WorkItem item;

        // 1. Try local pop (owner — wait-free)
        if (pop_local(item, worker_id)) {
            execute_actor(item);
            continue;
        }

        // 2. Try EDF (deadline-ordered)
        if (pop_edf(item, worker_id)) {
            execute_actor(item);
            continue;
        }

        // 3. Try stealing
        if (try_steal(item)) {
            execute_actor(item);
            continue;
        }

        // 4. No work — backoff
        backoff();
    }
}
```

### 4.3 `backoff()` Implementation

```cpp
void HybridScheduler::backoff() {
    // Exponential backoff with yield
    static thread_local uint32_t backoff_count = 0;
    uint32_t count = backoff_count++;

    if (count < 4) {
        std::this_thread::yield();
    } else {
        // Sleep for a short interval to avoid spinning
        std::this_thread::sleep_for(std::chrono::microseconds(10 * (count - 3)));
    }
}
```

---

## 5. `YieldAwaiter` — Cooperative Yield

### 5.1 `YieldAwaiter` Definition

```cpp
// include/hpactor/sched/yield_awaiter.hpp
#pragma once

#include <coroutine>
#include <cstdint>

namespace hpactor::sched {

// YieldAwaiter: co_await scheduler.yield() — voluntarily suspends
// and immediately re-enqueues the actor at the current priority.
class YieldAwaiter {
public:
    explicit YieldAwaiter(IScheduler* scheduler, ActorId actor_id,
                          uint8_t priority = 0) noexcept
        : scheduler_(scheduler), actor_id_(actor_id), priority_(priority) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;

        // Transition: Running → Ready (re-enqueue)
        // The continuation is NOT stored — we immediately schedule a wakeup

        // Schedule immediate re-execution at same priority
        scheduler_->notify_ready(actor_id_, priority_, INT64_MAX);

        // For now, we rely on notify_ready to cause the worker to pick up
        // the actor again. The continuation is not suspended long-term.
    }

    void await_resume() noexcept {
        // Just continuing — no special action needed
    }

private:
    IScheduler* scheduler_;
    ActorId actor_id_;
    uint8_t priority_;
    std::coroutine_handle<> continuation_;
};

} // namespace hpactor::sched
```

### 5.2 `IScheduler::yield()` Interface

Add to `IScheduler`:

```cpp
// In scheduler.hpp IScheduler interface
virtual void yield(ActorId actor, uint8_t priority) = 0;
```

Implementation in `HybridScheduler`:

```cpp
void HybridScheduler::yield(ActorId actor, uint8_t priority) {
    notify_ready(actor, priority, INT64_MAX);
}
```

---

## 6. Timer Integration — `TimerAwaiter` Wiring

### 6.1 Revised `TimerAwaiter`

```cpp
// include/hpactor/sched/coroutine_awaiters.hpp (update TimerAwaiter)
class TimerAwaiter {
public:
    TimerAwaiter(int64_t delay_ns, HybridScheduler& scheduler,
                 ActorId actor_id, uint8_t priority = 0) noexcept
        : scheduler_(scheduler), actor_id_(actor_id),
          delay_ns_(delay_ns), priority_(priority) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;

        // Schedule timer — on expiry, the actor is re-woken via notify_ready
        timer_id_ = scheduler_.schedule_timer(
            delay_ns_,
            [this] {
                scheduler_.notify_ready(actor_id_, priority_, INT64_MAX);
            }
        );

        // Transition to IOWaiting
        auto& promise = std::coroutine_handle<CoroutinePromise>::from_address(
                            continuation.address()).promise();
        promise.set_io_waiting();

        return true;
    }

    void await_resume() noexcept {
        // Timer fired — actor has been re-woken and will resume
    }

    bool await_cancel() noexcept {
        return scheduler_.cancel_timer(timer_id_);
    }

private:
    HybridScheduler& scheduler_;
    ActorId actor_id_;
    int64_t delay_ns_;
    uint8_t priority_;
    uint64_t timer_id_{0};
    std::coroutine_handle<> continuation_;
};
```

---

## 7. Continuation Resume — The Missing Link

### 7.1 The Wakeup Chain

```
MailboxAwaiter::await_suspend() stores continuation
        │
        ▼
Actor blocks on empty mailbox — state: Idle
        │
        ▼
Message arrives → ActorMailbox::enqueue()
        │
        ▼
ActorMailbox sees was_empty=true → CAS(true, false) + notify_ready()
        │
        ▼
Worker picks up actor → execute_actor()
        │
        ▼
resume() called on stored continuation
        │
        ▼
Actor code after co_await mailbox.receive() continues
```

### 7.2 Where `continuation.resume()` is Called

The `continuation` stored in `CoroutinePromise` is **only resumed from `execute_actor()` after the actor is picked up by a worker**. The edge-trigger from `ActorMailbox` schedules the actor, but the actual resume happens in the scheduler:

```cpp
// In execute_actor() — after resuming:
coroutine.resume();

// If the coroutine suspended via MailboxAwaiter, the continuation is stored.
// When the worker loop picks up this actor again (because ActorMailbox called
// notify_ready), execute_actor() is called again.
// At that point, await_ready() in MailboxAwaiter will return true (mailbox non-empty),
// and the coroutine will proceed past the co_await.
```

### 7.3 Continuation Storage in `CoroutinePromise`

```cpp
// In coroutine_task.hpp — CoroutinePromise holds the continuation
struct CoroutinePromise {
    // ...
    std::coroutine_handle<> continuation;  // stored on await_suspend

    // Called by ActorMailbox edge-trigger when message arrives during Idle
    void notify_mailbox_nonempty() {
        if (continuation && !continuation.done()) {
            continuation.resume();
        }
    }
};
```

**Key invariant**: The `continuation` is stored in the promise when the coroutine suspends via `await_suspend()`. When a message arrives and the actor is re-scheduled, `execute_actor()` calls `coroutine.resume()` which resumes from the suspension point (after `co_await mailbox.receive()`). The `await_ready()` check on re-entry will see the message is now available.

---

## 8. `BlockingMailboxAwaiter` + `CoroutineFramePool` Integration

### 8.1 Revised `BlockingMailboxAwaiter`

For actors that need blocking receive (deep call stacks), `BlockingMailboxAwaiter` works with stackful coroutines allocated from `CoroutineFramePool`:

```cpp
// include/hpactor/sched/coroutine_awaiters.hpp (BlockingMailboxAwaiter update)
class BlockingMailboxAwaiter {
public:
    BlockingMailboxAwaiter(CoroutinePromise& promise,
                           ActorMailbox<MessageNode>* mailbox,
                           std::coroutine_handle<> continuation) noexcept
        : promise_(promise), mailbox_(mailbox), continuation_(continuation) {}

    bool await_ready() const noexcept { return !mailbox_->was_empty(); }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        // Reset edge-trigger flag so next enqueue wakes
        mailbox_->set_was_empty(true);
        promise_.continuation = continuation;
        promise_.set_idle();
        return true;
    }

    MessageVariant await_resume() noexcept {
        // Pop and return the message
        auto* node = mailbox_->dequeue();
        return std::move(node->payload);
    }

private:
    CoroutinePromise& promise_;
    ActorMailbox<MessageNode>* mailbox_;
    std::coroutine_handle<> continuation_;
};
```

### 8.2 Frame Pool Integration in `WorkerThread`

```cpp
// src/sched/worker_thread.cpp
// WorkerThread already has: memory::CoroutineFramePool frame_pool_{512, 256};

// On worker start, set the thread-local pointer:
void WorkerThread::start() {
    // Expose frame pool to coroutine allocations
    tl_frame_pool = &frame_pool_;
    // ...
}

// On worker stop:
void WorkerThread::stop() {
    tl_frame_pool = nullptr;
    // ...
}
```

---

## 9. State Transition Summary

### 9.1 Valid Transitions

```
Idle          --message arrived + CAS--> Ready
Ready         --worker picks up + CAS--> Running
Running       --co_await (mailbox empty) --> Idle          (via MailboxAwaiter)
Running       --co_await (timer) ---------> IOWaiting      (via TimerAwaiter)
Running       --co_await (yield) ---------> Ready          (via YieldAwaiter)
Running       --co_return ---------------)> Terminated
IOWaiting     --timer fires -------------> Ready          (via TimerCallback)
IOWaiting     --I/O completes -----------> Ready          (via EventLoop callback)
```

### 9.2 CAS Failure Handling

If a CAS fails (transition wasn't valid), the operation is a no-op:
- `Running → Idle` CAS fails → actor was already suspended by another path; skip
- `Ready → Running` CAS fails → actor was already picked up by another worker; skip
- `Idle → Ready` CAS fails → another enqueue already claimed the wakeup; skip

---

## 10. Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `include/hpactor/sched/actor_coroutine.hpp` | **Create** | `ActorCoroutine` wrapper class |
| `include/hpactor/sched/yield_awaiter.hpp` | **Create** | `YieldAwaiter` for cooperative yield |
| `include/hpactor/mailbox/actor_mailbox.hpp` | **Create** | `ActorMailbox` with edge-trigger wakeup |
| `include/hpactor/actor/event_based_actor.hpp` | **Modify** | Add `act()`, `ActorMailbox`, `ActorCoroutine` |
| `src/actor/event_based_actor.cpp` | **Modify** | Implement `ensure_coroutine_started()`, `mailbox_has_messages()` |
| `src/sched/scheduler.cpp` | **Modify** | Implement `execute_actor()` with `resume()`, `backoff()` |
| `include/hpactor/sched/scheduler.hpp` | **Modify** | Add `yield()`, `current_worker()`, `set_current_worker()` |
| `src/sched/scheduler.cpp` | **Modify** | Add `yield()`, worker thread-local pointer |

---

## 11. Test Strategy

### 11.1 Unit Tests

| Test | What It Verifies |
|------|-----------------|
| `test_coroutine_task_lifecycle` | `CoroutineTask` move/destroy/resume/done |
| `test_actor_state_cas` | All valid/invalid CAS transitions |
| `test_mailbox_awaiter_suspend` | `await_ready()` false → `await_suspend()` stores continuation |
| `test_mailbox_edge_trigger` | First enqueue after empty calls `notify_ready`; second does not |
| `test_yield_awaiter` | `yield()` causes re-schedule at same priority |
| `test_timer_awaiter` | Timer expiry calls `notify_ready` |
| `test_execute_actor_resume` | `execute_actor()` calls `resume()` on coroutine |
| `test_actor_mailbox_dequeue` | `dequeue()` resets edge-trigger flag when empty |

### 11.2 Integration Test

```
PingActor (coroutine-based)
        │
        ▼
Spawn PingActor
        │
        ▼
Send Ping message
        │
        ▼
Actor wakes up (edge-trigger)
        │
        ▼
Handles Ping, sends Pong
        │
        ▼
co_return on Stop → Terminated
```

---

## 12. Open Questions

1. **Thread-safety of continuation**: When `ActorMailbox::enqueue()` is called from an arbitrary thread and triggers `notify_ready()`, is it safe to call `continuation.resume()` from that thread, or must it be deferred to the actor's owner worker? The spec currently defers via `notify_ready()` → worker loop.

2. **Multiple messages batched**: If 10 messages arrive while an actor is idle, `ActorMailbox` will call `notify_ready()` 10 times. The CAS on `mailbox_was_empty` ensures only the first enqueue triggers wakeup — but does the actor need to drain the entire mailbox on wakeup, not just process one message?

3. **`yield()` priority**: Should `yield()` re-enqueue at the same priority, or always at a lower priority to prevent starvation?

4. **Actor migration**: If an actor's owner worker is blocked (I/O), should the actor be migratable to another worker, or does it stay with its owner?

---

## References

- `docs/superpowers/specs/2026-04-15-coroutine-scheduling-design.md` — Phase 1-5 architecture
- `include/hpactor/sched/coroutine_task.hpp` — existing `CoroutineTask` / `CoroutinePromise`
- `include/hpactor/sched/coroutine_awaiters.hpp` — existing awaiter types
- `include/hpactor/mailbox/mpsc_mailbox.hpp` — existing `MPSCMailbox`
- `src/sched/scheduler.cpp:173` — current stub `execute_actor()`
