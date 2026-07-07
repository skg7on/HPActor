# HPActor Coroutine Architecture Design

## Executive Summary

HPActor actors are C++20 stackless coroutines scheduled M:N onto a pool of worker threads via a work-stealing HybridScheduler. The coroutine subsystem provides the suspension/resumption machinery that allows actors to `co_await` messages, timers, and cooperative yields — all without blocking worker threads. This document describes the architecture as implemented in the current codebase at `include/hpactor/coroutine/` and `src/coroutine/`.

**Status:** Fully implemented and production-integrated. The coroutine pipeline is wired through `HybridScheduler::execute_actor()`, `EventBasedActor::act()`, and the `MPSCActorMailbox` edge-trigger wakeup chain.

---

## 1. Design Goals

1. **M:N Mapping**: Map M user-space actor coroutines onto N worker threads with minimal context-switch overhead.
2. **Two-Level Scheduling**: Decouple message delivery (mailbox enqueue → scheduler notify) from CPU execution (worker picks up → resume coroutine).
3. **Cooperative Multitasking**: Actors voluntarily yield after processing a message; no preemption needed.
4. **Edge-Trigger Wakeup**: A CAS-based mailbox flag ensures only the first message after an empty mailbox wakes the actor — no spurious wakeups.
5. **Cache Locality**: The HybridScheduler's A2WS victim selection prefers keeping actors on their last worker.
6. **No Exceptions / No RTTI**: All error paths use `std::error_code` or typed enums; promise types handle `unhandled_exception()` safely.
7. **Narrow Scheduler Interfaces**: Awaiters depend on segregated interfaces (`ITimerService`, `IActorReadyNotifier`, `IActorYieldScheduler`), not the concrete `HybridScheduler`. This follows the scheduler decoupling architecture.

---

## 2. Core Abstractions

### 2.1 CoroutinePromise

`CoroutinePromise` is the `promise_type` for actor coroutines. It owns the actor's lifecycle state, mailbox pointer, and the continuation handle used for wakeup.

```
include/hpactor/coroutine/coroutine_task.hpp
```

| Field | Purpose |
|-------|---------|
| `ActorId actor_id` | Identity for scheduler routing |
| `ActorState* actor_state` | Pointer to shared state — may alias `EventBasedActor::actor_state_` |
| `WorkerThread* owner` | Worker that currently executes this actor |
| `void* mailbox` | Opaque pointer to `MPSCActorMailbox<T>` |
| `std::atomic<bool> mailbox_was_empty` | Edge-trigger flag for wakeup CAS |
| `std::coroutine_handle<> continuation` | Stored on suspend; resumed on wakeup |

**Key design decision — `actor_state` pointer indirection:** Instead of embedding an `ActorState` directly, the promise holds a pointer that by default points to a local fallback, but can be redirected to `EventBasedActor::actor_state_`. This allows both the promise and the actor object to observe and transition the same state machine.

**Key design decision — `final_suspend` keeps frame alive:** The promise returns `std::suspend_always` from `final_suspend()`, which prevents the compiler from automatically destroying the coroutine frame. This allows the actor runtime to call `resume()` again to re-enter `act()` after a restart cycle. The actor runtime explicitly destroys the frame via `coroutine_handle::destroy()` when the actor is permanently terminated.

### 2.2 CoroutineTask

`CoroutineTask` wraps `std::coroutine_handle<CoroutinePromise>` with move-only semantics.

```cpp
class CoroutineTask {
    handle_type handle_;  // std::coroutine_handle<CoroutinePromise>
public:
    void resume();        // Resume if not done
    bool done() const;    // Check completion
    explicit operator bool() const;
};
```

Three specializations of `std::coroutine_traits` wire `CoroutineTask` as a valid coroutine return type with up to one by-reference parameter (used when `act()` receives the actor context reference).

**C++17 fallback:** When `HPACTOR_SUPPORT_COROUTINES` is not defined, stub types are provided so the rest of the type system (`ActorState`, `ActorId`) compiles without `<coroutine>`.

### 2.3 ActorCoroutine

`ActorCoroutine` is a lightweight wrapper that pairs a `CoroutineTask` with its `ActorId`. It is produced by `EventBasedActor::act()` and consumed by `HybridScheduler::execute_actor()`.

```
include/hpactor/coroutine/actor_coroutine.hpp
```

```cpp
class ActorCoroutine {
    CoroutineTask task_;
    ActorId actor_id_;
public:
    void resume();                    // Forward to task_.resume()
    bool done() const;
    CoroutinePromise& promise();      // Access state
};
```

### 2.4 ActorState Machine

```
include/hpactor/actor/actor_state.hpp
```

```
                         spawn()
                           │
                           ▼
                      ┌─────────┐
        ┌─────────────│  Idle   │◀──────────────────────┐
        │             └────┬────┘                       │
        │                  │ first message              │ mailbox empty
        │                  │ (edge-trigger CAS)         │ + await_suspend
        │                  ▼                            │
        │             ┌─────────┐                       │
        │             │  Ready  │───────────────────────┤
        │             │(queued) │                       │
        │             └────┬────┘                       │
        │                  │ worker picks up            │
        │                  ▼                            │
        │             ┌─────────┐                       │
        │             │ Running │───────────────────────┘
        │             └────┬────┘
        │                  │
        │   co_await       │ TimerAwaiter
        │   timer/IO       ▼
        │             ┌──────────┐
        └─────────────│IOWaiting │───(timer fires)──▶ Ready
                      └──────────┘
        │
        │ co_return or unhandled_exception
        ▼
  ┌─────────────┐
  │ Terminated  │  (frame kept alive by final_suspend)
  └─────────────┘
```

States are single-bit flags in a `std::atomic<uint32_t>` with CAS-based transitions:

| State | Bit | Meaning |
|-------|-----|---------|
| `kIdle` | 0x01 | Suspended, mailbox empty — waiting for message |
| `kReady` | 0x02 | In scheduler queue — has pending messages |
| `kRunning` | 0x04 | Currently executing on a worker thread |
| `kIOWaiting` | 0x08 | Waiting for timer or I/O completion |
| `kTerminated` | 0x10 | Coroutine has finished (co_return or error) |

**Valid transitions (all via CAS):**

| From | To | Trigger |
|------|----|---------|
| Idle | Ready | First message enqueued; `notify_ready()` called |
| Ready | Running | Worker picks up item; `execute_actor()` |
| Running | Idle | `MailboxAwaiter::await_suspend()` — mailbox empty |
| Running | Ready | `YieldAwaiter::await_suspend()` — cooperative yield |
| Running | IOWaiting | `TimerAwaiter::await_suspend()` — timer scheduled |
| Running | Terminated | `co_return` or `unhandled_exception()` |
| IOWaiting | Ready | Timer fires; callback calls `notify_ready()` |

Invalid CAS transitions (e.g., `Idle → Running` directly) fail the CAS and are silently ignored — this is the primary defense against duplicate scheduling.

---

## 3. Awaiter System

All awaiters live in `include/hpactor/coroutine/coroutine_awaiters.hpp` and are gated by `#if HPACTOR_SUPPORT_COROUTINES`.

### 3.1 MailboxAwaiter\<T\>

The core suspend/resume primitive. An actor calls `co_await MailboxAwaiter<T>(promise, mailbox, dlq, metrics, actor_id)` to wait for the next message.

```
MailboxAwaiter<T>::await_ready()
    │
    ├─ mailbox non-empty → return true (don't suspend)
    │
    └─ mailbox empty → return false → await_suspend()
            │
            ├─ Check was_empty() again (defend against lost wakeup race)
            │   └─ If non-empty now → return false (don't suspend)
            │
            ├─ Reset edge-trigger: set_was_empty(true)
            │
            ├─ CAS actor_state: Running → Idle
            │   └─ Success → store continuation, return true (suspend)
            │   └─ Failure → return false (terminated or already scheduled)
            │
            └─ ... worker loop later picks up actor via notify_ready ...
                    │
                    ▼
            await_resume()
                │
                ├─ Dequeue from mailbox
                ├─ Check deadline → if expired: emit metric, record DLQ, loop
                └─ Return valid message to actor
```

**Lost wakeup race defense:** Between `await_ready()` returning false and `await_suspend()` executing, a message may arrive. The sender does `was_empty=true → CAS(true, false) → notify_ready()`. The awaiter defends against this by re-checking `was_empty()` inside `await_suspend()` before committing to suspend. If a message arrived in the window, `await_suspend()` returns `false` (don't suspend) and the actor proceeds directly to `await_resume()`.

**Deadline enforcement:** `await_resume()` checks `msg->deadline_ns()` against `steady_clock::now()`. Expired messages are dropped with:
- A `kDeliveryExpired` metric event emitted to the ring buffer
- A `DeadLetterRecord` pushed to the DLQ (if enabled)
- The message destructor called and memory deallocated
- Loop continues to try the next message

This is a production-grade feature not in the original design spec — it was added during implementation to handle TTL expiry at the point of consumption.

**Template parameter `T`:** The awaiter is templated on the mailbox's message type (typically `TypedMessage`), allowing it to `dequeue()` and return the correct type.

### 3.2 TimerAwaiter

Suspends the actor for a configurable delay, then resumes via a timer callback.

```
TimerAwaiter::await_suspend()
    │
    ├─ Set promise state to IOWaiting
    ├─ Call timer_service_.schedule_after(callback, delay_ns)
    │   └─ callback: [this] { ready_notifier_.notify_ready(actor_id, priority, INT64_MAX); }
    └─ Return true (suspend)

TimerAwaiter::await_resume()
    └─ Timer fired; actor has been re-woken by scheduler
```

The awaiter depends on two narrow interfaces rather than `HybridScheduler` directly:
- `ITimerService` — provides `schedule_after()` / `cancel_timer()`
- `IActorReadyNotifier` — provides `notify_ready()`

This follows the Interface Segregation Principle from the scheduler decoupling work.

### 3.3 YieldAwaiter

Cooperative yield: the actor voluntarily gives up the CPU after processing one message so other actors can run. The actor is immediately re-enqueued at the same priority.

```
include/hpactor/coroutine/yield_awaiter.hpp
```

```cpp
class YieldAwaiter {
    IActorYieldScheduler* scheduler_;
    // await_ready() → always false
    // await_suspend() → scheduler_->yield(actor_id, priority) → re-enqueue
    // await_resume() → no-op
};
```

`SchedulerYield` is a convenience variant that extracts `actor_id` from the coroutine's own promise, avoiding the need to thread `ActorId` through user code.

### 3.4 BlockingMailboxAwaiter\<T\>

For stackful coroutines (blocking actors that use `CoroutineFramePool`). Suspends the actor and returns the dequeued message on resume. Uses the same lost-wakeup race defense as `MailboxAwaiter<T>`.

---

## 4. Edge-Trigger Mailbox Wakeup

### 4.1 The Problem

When a message arrives at an idle actor's mailbox, the actor must be woken up. But calling `notify_ready()` on every enqueue would waste scheduler cycles — if the actor is already running or queued, subsequent messages don't need to re-schedule.

### 4.2 MPSCActorMailbox\<T\>

```
include/hpactor/mailbox/mpsc_actor_mailbox.hpp
```

The `MPSCActorMailbox<T>` wraps the lock-free `MPSCMailbox<T>` queue with a CAS-based edge-trigger flag `mailbox_was_empty`:

```
Producer: enqueue(node)
    │
    ├─ Check was_empty BEFORE enqueue
    ├─ mailbox_.enqueue(node)    // lock-free Vyukov MPSC push
    └─ If was_empty:
           CAS mailbox_was_empty: true → false
           │
           ├─ Success → scheduler_->notify_ready(actor_id, priority, deadline)
           │   (We claimed the wakeup — actor will be picked up)
           │
           └─ Failure → another enqueue already claimed the wakeup (no-op)

Consumer: dequeue()
    │
    ├─ node = mailbox_.dequeue()  // lock-free single-consumer pop
    └─ If node != nullptr AND now empty:
           mailbox_was_empty.store(true)  // reset edge-trigger for next batch
```

**Why CAS?** Multiple producers may enqueue concurrently. The CAS ensures exactly one producer claims the right to call `notify_ready()` on the empty→non-empty transition. Subsequent enqueues while the mailbox is non-empty see `was_empty=false` and skip the CAS entirely — no atomic contention.

**`inject_for_test()`:** A bypass method that enqueues without edge-trigger, used by tests that need to prepopulate mailboxes without scheduler interference.

---

## 5. Scheduler Integration

### 5.1 The Full Wakeup Chain

```
Message arrives at MPSCActorMailbox
        │
        ▼
MPSCActorMailbox::enqueue() — sees was_empty=true
        │
        ▼
CAS mailbox_was_empty: true → false (claims wakeup)
        │
        ▼
scheduler_->notify_ready(actor_id, priority, deadline)
        │
        ├─ EDF queue upsert (for deadline tracking)
        └─ Push WorkItem to target worker's MultiPriorityWorkQueue
                │
                ▼
Worker picks up WorkItem (local pop → EDF pop → steal → yield)
        │
        ▼
HybridScheduler::execute_actor(item)
        │
        ├─ actor->ensure_coroutine_started()  (lazy — first time only)
        ├─ CAS actor_state: Ready → Running
        │   └─ Failure → skip (already running/terminated)
        ├─ coroutine.resume()
        │       │
        │       ▼
        │   Actor coroutine body (act()) runs
        │       │
        │       ├─ co_await MailboxAwaiter → suspend (Idle)
        │       ├─ co_await TimerAwaiter → suspend (IOWaiting)
        │       ├─ co_await YieldAwaiter → self-notify_ready (Ready)
        │       └─ co_return → Terminated
        │
        └─ Post-resume check:
                ├─ Idle → wait for mailbox edge-trigger
                ├─ IOWaiting → wait for timer/IO callback
                └─ Terminated → actor->on_exit()
```

### 5.2 Lazy Coroutine Creation

`EventBasedActor::ensure_coroutine_started()` is called on the first scheduler pickup, not at spawn time. This keeps spawn cheap (no coroutine frame allocation) and defers frame creation to the worker thread that will own the actor.

### 5.3 Narrow Scheduler Interfaces

The coroutine subsystem does not depend directly on `HybridScheduler`. Instead, awaiters depend on segregated interfaces defined in `include/hpactor/sched/scheduler_interfaces.hpp`:

| Interface | Provides | Used By |
|-----------|----------|---------|
| `ITimerService` | `schedule_after()`, `cancel_timer()` | `TimerAwaiter` |
| `IActorReadyNotifier` | `notify_ready()` | `TimerAwaiter` (callback context) |
| `IActorYieldScheduler` | `yield()` | `YieldAwaiter`, `SchedulerYield` |

These interfaces are a product of the scheduler decoupling work (2026-05-29). The concrete `HybridScheduler` implements all of them, but awaiters are testable against mocks without spinning up a full scheduler.

---

## 6. CoroutineFramePool

```
include/hpactor/adt/coroutine_frame_pool.hpp
src/coroutine/coroutine_frame_pool.cpp
```

A **fixed-size pre-allocated pool** for stackful coroutine frames. Event-based actors use **stackless** coroutines (compiler-allocated frames on the heap) and do not use this pool. The pool exists for blocking actors that need explicit stack allocation.

### Pool Characteristics

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `num_frames` | Construction-time | Bounded pool size |
| `stack_size` | 8 KiB | Per-frame stack allocation |
| Acquisition | O(1) lock-free CAS pop from free stack |
| Release | O(1) lock-free CAS push to free stack |
| Exhaustion | `acquire()` returns `nullptr` — caller falls back to heap |

### Structure

```
CoroutineFramePool
    ├── std::vector<std::unique_ptr<std::byte[]>> stacks_   // owned allocations
    ├── std::vector<Frame> frames_                           // metadata array
    │       ├── stack_ptr, stack_size, in_use
    └── std::atomic<FreeNode*> free_stack_                   // CAS-based freelist
            └── FreeNode { next*, index }
```

Each `Frame` is at a fixed index. The free stack holds indices, not pointers to frames — this enables O(1) frame-to-index lookup during release.

**Namespace:** The concrete implementation lives in `hpactor::adt` (shared ADT layer). The coroutine header `include/hpactor/coroutine/coroutine_frame_pool.hpp` provides a namespace alias: `namespace hpactor::sched { using hpactor::adt::CoroutineFramePool; }`.

---

## 7. Actor Entry Point

### 7.1 EventBasedActor::act()

The `act()` method is the coroutine entry point for event-based actors:

```cpp
class EventBasedActor : public LocalActor {
public:
    virtual CoroutineTask act();  // Override to define actor behavior
    ActorCoroutine& get_actor_coroutine();
    void ensure_coroutine_started();
    // ...
private:
    ActorCoroutine coroutine_;
    ActorState actor_state_;       // shared with CoroutinePromise
};
```

The default `act()` returns `co_return;` — an immediately-terminating coroutine for actors that use the legacy `make_behavior()` path instead.

### 7.2 Example: Coroutine-Based Actor

```cpp
class PingActor : public EventBasedActor {
public:
    CoroutineTask act() override {
        while (true) {
            auto msg = co_await MailboxAwaiter<TypedMessage>(
                *context()->promise(),
                context()->actor_mailbox());

            std::visit(overloaded{
                [&](const Ping& ping) {
                    context()->send(ping.from, Pong{});
                },
                [&](const Stop&) {
                    co_return;  // → Terminated
                }
            }, msg.payload());
        }
    }
};
```

### 7.3 Dual Path: Coroutine vs. Behavior

`EventBasedActor` supports both the coroutine path (`act()`) and the legacy behavior path (`make_behavior()` / `receive()`). During migration, both coexist:

- Coroutine path: `act()` → `co_await MailboxAwaiter` → process message
- Behavior path: `make_behavior()` returns `Behavior` → `receive()` invokes handler

The scheduler checks for the coroutine first in `execute_actor()`. If the actor has no coroutine (legacy path), it falls through to the behavior-based `receive()` path.

---

## 8. Implementation vs. Original Design Spec

The implemented system diverges from the original design spec (`2026-04-15-coroutine-scheduling-design.md` and `2026-04-17-coroutine-context-switching-design.md`) in several important ways. These divergences represent architectural improvements made during implementation:

| Aspect | Original Spec | Implemented |
|--------|--------------|-------------|
| Header location | `include/hpactor/sched/` | `include/hpactor/coroutine/` (per header placement rules) |
| Scheduler dependency | Direct `HybridScheduler&` in awaiters | Narrow interfaces (`ITimerService`, `IActorReadyNotifier`, `IActorYieldScheduler`) |
| State ownership | Promise owns `ActorState` directly | `ActorState*` pointer — aliases `EventBasedActor::actor_state_` |
| `final_suspend` | Not specified | `suspend_always` — keeps frame alive for restart |
| C++17 support | Not considered | Full `#if HPACTOR_SUPPORT_COROUTINES` stub path |
| Deadline checking | Not in spec | `MailboxAwaiter::await_resume()` checks TTL, emits metrics, records DLQ |
| CoroutineFramePool | In `sched/` | In `adt/` (shared ADT) with `sched` namespace alias |
| Mailbox type | Simple `MPSCMailbox<T>` | `MPSCActorMailbox<T>` with edge-trigger CAS |
| `CoroutinePromise::notify_mailbox_nonempty` | Resumes continuation directly | Exists but primary wakeup path is through `notify_ready()` → worker loop |

---

## 9. File Map

### Public Headers (`include/hpactor/coroutine/`)

| File | Purpose |
|------|---------|
| `coroutine_task.hpp` | `CoroutinePromise`, `CoroutineTask`, `std::coroutine_traits` specializations, C++17 fallback stubs |
| `coroutine_awaiters.hpp` | `MailboxAwaiter<T>`, `TimerAwaiter`, `BlockingMailboxAwaiter<T>` — all with edge-trigger race defense |
| `coroutine_frame_pool.hpp` | Namespace alias: `hpactor::sched::CoroutineFramePool` → `hpactor::adt::CoroutineFramePool` |
| `yield_awaiter.hpp` | `YieldAwaiter`, `SchedulerYield` — cooperative yield for fair scheduling |
| `actor_coroutine.hpp` | `ActorCoroutine` — owns `CoroutineTask` + `ActorId` |

### Implementation (`src/coroutine/`)

| File | Purpose |
|------|---------|
| `coroutine_frame_pool.cpp` | Lock-free CAS freelist for pre-allocated stack frames |

### Supporting Types (other locations)

| File | Purpose |
|------|---------|
| `include/hpactor/actor/actor_state.hpp` | `ActorState` — atomic state bitmask with CAS transitions |
| `include/hpactor/adt/coroutine_frame_pool.hpp` | `adt::CoroutineFramePool` — concrete pool implementation |
| `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` | `MPSCActorMailbox<T>` — MPSC queue + edge-trigger wakeup |
| `include/hpactor/sched/scheduler_interfaces.hpp` | `ITimerService`, `IActorReadyNotifier`, `IActorYieldScheduler` |
| `include/hpactor/actor/event_based_actor.hpp` | `act()`, `ActorCoroutine`, `ActorState` member |

---

## 10. Design Decisions & Rationale

| Decision | Rationale |
|----------|-----------|
| C++20 stackless coroutines (not stackful) | Zero heap allocation for small frames; compiler optimizes frame layout; standard `<coroutine>` instead of Boost.Context |
| `final_suspend` keeps frame alive | Enables actor restart without re-allocating coroutine frame; supervisor-triggered restart just calls `resume()` again |
| Edge-trigger CAS, not level-trigger | Avoids O(N) scheduler calls for N enqueues to a running actor; only the first enqueue after empty triggers wakeup |
| Narrow scheduler interfaces | Decouples coroutine subsystem from concrete scheduler; awaiters testable without full `HybridScheduler`; follows ISP from scheduler decoupling work |
| `ActorState*` pointer indirection | Promise and `EventBasedActor` share the same state machine; transitions visible to both without synchronization |
| C++17 fallback path | Allows compilation on toolchains without `<coroutine>` support; stub types preserve type-system compatibility |
| Deadline checking at consume time (not enqueue time) | Avoids clock synchronization issues; actor-local `steady_clock` ensures monotonic deadline comparison |
| `CoroutineFramePool` in `adt/` | The pool is a general-purpose ADT, not specific to coroutine scheduling; the `coroutine/` header provides a namespace alias |
| `inject_for_test()` bypass | Test infrastructure needs to prepopulate mailboxes without triggering scheduler wakeup; avoids `set_scheduler_threads=0` workarounds |

---

## 11. Related Documentation

- [Actor Concurrency & Lock-Free Mailbox Rules](../mailbox/actor-concurrency-and-lockfree-mailbox-rules.md) — normative rules for MPSC mailbox correctness, actor state ownership, and ready-gate transitions
- [Scheduler Decoupling Design](../../superpowers/specs/2026-05-29-scheduler-decoupling-design.md) — narrow interface extraction and `ActorExecutionEngine`
- [Coroutine Scheduling Design Spec](../../superpowers/specs/2026-04-15-coroutine-scheduling-design.md) — original Phase 1-5 architecture
- [Coroutine Context Switching Design Spec](../../superpowers/specs/2026-04-17-coroutine-context-switching-design.md) — original missing-pipeline design (execute_actor, yield, edge-trigger)
- [Coroutine Scheduling Implementation Plan](../../superpowers/plans/2026-04-15-coroutine-scheduling-impl.md) — 7-phase build plan
- [Coroutine Context Switching Implementation Plan](../../superpowers/plans/2026-04-17-coroutine-context-switching-impl.md) — 9-phase pipeline build plan
- [Actor Core Concept](actor-core-concept.md) — actor model, type hierarchy, message system overview
