# ActorContext::schedule() — Timer-Based Message Delivery Design

**Date:** 2026-05-17
**Status:** Draft
**Issue:** [#97](https://github.com/skg7on/HPActor/issues/97)
**Dependency for:** #100 (OpsProbeActor periodic tick)

---

## Overview

Implement the currently stubbed `ActorContext::schedule(std::chrono::milliseconds, TypedMessage)` to deliver a message to the calling actor's own mailbox after a specified delay. Wire it through the existing `IScheduler::schedule_after()` / `TimingWheel` infrastructure.

## Current State

```cpp
// src/actor/actor_context.cpp:213-218
void ActorContext::schedule(std::chrono::milliseconds delay, TypedMessage msg) {
    // TODO: schedule message via actor system's clock/alarm mechanism
    (void)delay;
    (void)msg;
}
```

Return type is `void`. No cancellation mechanism exists. The order platform `--scenario payment-timeout` path schedules a 200ms timeout to detect unresponsive payment actors, but the message is silently discarded.

## Goals

- Deliver `msg` to the calling actor's mailbox after `delay` elapses
- Use the existing `IScheduler::schedule_after()` and `TimingWheel` infrastructure — no new timer subsystem
- The scheduled message carries the calling actor's own address as sender (self-send)
- Return `AlarmHandle` so callers can cancel scheduled messages
- No new external dependencies
- Thread-safe: callback fires on timer thread, enqueues via lock-free mailbox path

## Non-Goals

- Per-message deadline or priority scheduling — future phase
- Persistent timers surviving actor restart — future phase
- `schedule_every()` / recurring timer API on ActorContext — callers self-reschedule via `schedule()` in their handler

---

## Architecture

### Component Diagram

```
ActorContext::schedule(delay, msg)
    │
    ├─ Sets msg.sender_address() = own ActorAddress
    ├─ Captures [system_, own_id_, msg = std::move(msg)] in lambda
    ├─ Calls system_->scheduler()->schedule_after(lambda, delay_ns)
    └─ Returns AlarmHandle{timer_handle.id}

Timer thread (HybridScheduler::start)
    │
    ├─ TimingWheel::advance(now)
    │   └─ Fires expired callback
    └─ Lambda: system_->deliver_local(own_id_, msg)
        ├─ MPSCActorMailbox::try_push()  [lock-free, cross-thread safe]
        └─ scheduler->notify_ready()     [wakes worker thread]
```

### Data Flow

```
schedule(200ms, PaymentTimedOut)
    │
    ├─ Encode: msg.sender = self.address(), msg.type = PaymentTimedOutTag
    ├─ Wrap:   lambda = [sys, id, msg] { sys->deliver_local(id, msg); }
    ├─ Insert: timing_wheel.schedule(200_000_000ns, lambda) → id=42
    └─ Return: AlarmHandle{42}
    
    ... 200ms elapses ...
    
Timer thread advance()
    └─ callback fires
        └─ deliver_local(coordinator_id, PaymentTimedOut)
            └─ mpsc_mailbox.try_push(msg, meta)
                └─ scheduler.notify_ready(coordinator_id)
                    └─ coordinator processes PaymentTimedOut in receive()
```

### API Change

**Before:**
```cpp
void schedule(std::chrono::milliseconds delay, TypedMessage msg);
```

**After:**
```cpp
AlarmHandle schedule(std::chrono::milliseconds delay, TypedMessage msg);
void cancel_schedule(AlarmHandle handle);
```

`AlarmHandle` (already defined at `types.hpp:401-412`) wraps a `uint64_t id_`. It currently has no use in the codebase — this gives it a purpose. The `cancel_schedule()` method calls `system_->scheduler()->cancel_timer(TimerHandle{handle.id()})`.

---

## Detailed Design

### 1. ActorContext::schedule() implementation

**File:** `src/actor/actor_context.cpp`

```cpp
AlarmHandle ActorContext::schedule(std::chrono::milliseconds delay,
                                   TypedMessage msg) {
    auto* sched = system_->scheduler();
    if (!sched) return AlarmHandle{};

    // Self-send: the message appears to come from the calling actor.
    msg.set_sender_address(owner_->address());

    ActorId self_id = owner_->id();
    ActorSystem* sys = system_;

    auto callback = [sys, self_id, msg = std::move(msg)]() mutable {
        sys->deliver_local(self_id, std::move(msg));
    };

    int64_t delay_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
    auto handle = sched->schedule_after(std::move(callback), delay_ns);
    return AlarmHandle{handle.id};
}
```

Key design decisions:

- **`msg.set_sender_address(owner_->address())`** — the scheduled message appears to come from the actor itself. This matches the existing `context()->reply()` pattern where the reply sender is set to the replying actor.
- **Capture by move** — the `TypedMessage` is moved into the lambda, avoiding copies. The message lives on the heap inside `std::function`'s type-erased storage.
- **`deliver_local()`** — reuses the full delivery path (mailbox enqueue + scheduler notification). This is the same function called by `context()->send()` for local delivery. It is thread-safe because `MPSCActorMailbox::try_push()` is lock-free and cross-thread safe.
- **No `weak_ptr` check** — scheduled messages for an actor that has terminated will be delivered to a closed mailbox. `MPSCActorMailbox::try_push()` returns `MailboxClosed` for terminated actors, and the message is silently dropped (or routed to DLQ if configured). This is acceptable — no use-after-free.
- **`AlarmHandle` return** — zero-value `AlarmHandle` signals failure (null scheduler). The caller can pass the handle to `cancel_schedule()` to cancel before the timer fires.

### 2. ActorContext::cancel_schedule() implementation

```cpp
void ActorContext::cancel_schedule(AlarmHandle handle) {
    if (!handle.valid()) return;
    auto* sched = system_->scheduler();
    if (!sched) return;
    sched->cancel_timer(sched::TimerHandle{handle.id()});
}
```

Cancellation is best-effort:
- If the timer has already fired (callback is executing or has executed), `cancel_timer()` returns `false` and the message will be or has been delivered — this is harmless.
- If the timer is still pending, it is removed from the timing wheel and will never fire.

### 3. Concurrency Contract

| Thread | Action | Safety |
|--------|--------|--------|
| Actor worker thread | Calls `schedule()` | Safe: `schedule_after()` inserts into `TimingWheel` under a mutex |
| Timer thread | Fires callback, calls `deliver_local()` | Safe: `MPSCActorMailbox::try_push()` is lock-free, cross-thread |
| Actor worker thread | Calls `cancel_schedule()` | Safe: `cancel_timer()` removes from `TimingWheel` under a mutex |
| Any thread | Concurrent `schedule()` + `cancel_schedule()` on different handles | Safe: independent timer entries |

The callback MUST NOT block — it runs on the timer advancement thread. `deliver_local()` satisfies this: it only does lock-free CAS operations and a `notify_ready()` call (another CAS on the scheduler's ready queue).

### 4. Interaction with Actor Lifecycle

| Actor State | schedule() behavior | Delivery behavior |
|-------------|-------------------|-------------------|
| Running | Normal: timer registered | Normal: message enqueued |
| Draining | Normal: timer registered | Message enqueued and processed (drain-aware handlers decide) |
| Stopping/Stopped | Normal: timer registered | Mailbox closed: message dropped or DLQ'd |
| After destruction | Not possible (ActorContext lifetime ≤ actor lifetime) | — |

No `weak_ptr` check is needed in the callback. The `ActorContext`'s lifetime is bounded by the actor's lifetime (it is owned by `LocalActor`). The `ActorSystem*` outlives all actors. If the actor has terminated, `deliver_local()` finds a closed mailbox and drops the message — no dangling pointer access.

---

## Changes Summary

### Files Modified

| File | Change |
|------|--------|
| `include/hpactor/actor_context.hpp` | Change `schedule()` return type from `void` to `AlarmHandle`; add `cancel_schedule(AlarmHandle)` declaration |
| `src/actor/actor_context.cpp` | Implement `schedule()` and `cancel_schedule()` |
| `tests/actor/test_actor_context.cpp` | New test: `test_schedule_delivers_after_delay` |

### Files Referenced (no changes)

| File | Role |
|------|------|
| `include/hpactor/sched/scheduler.hpp` | `IScheduler::schedule_after()`, `TimerHandle` |
| `include/hpactor/types/types.hpp` | `AlarmHandle` (already defined) |
| `src/sched/scheduler.cpp` | `HybridScheduler::schedule_after()` implementation |
| `src/sched/timing_wheel.cpp` | `TimingWheel::schedule()` implementation |

---

## Acceptance Criteria

1. `ActorContext::schedule()` returns a valid `AlarmHandle` (non-zero id) when called with a running scheduler
2. The scheduled message is delivered to the calling actor's mailbox after the specified delay (±50ms tolerance in tests)
3. `cancel_schedule(handle)` cancels a pending timer — the message is never delivered
4. `cancel_schedule()` on an already-fired timer is a no-op (no crash)
5. `cancel_schedule(AlarmHandle{})` (invalid handle) is a no-op
6. The order platform `--scenario payment-timeout` prints `status=payment_timed_out`
7. No new external dependencies, no exceptions, no RTTI
8. 140 existing tests continue to pass

## Test Plan

### test_schedule_delivers_after_delay

1. Create `ActorSystem` with `scheduler_threads = 2` (needs timer thread running)
2. Spawn a test actor that, in its behavior, schedules a self-message with 50ms delay and stores the current time
3. When the scheduled message arrives, the actor records the elapsed time and signals completion
4. Poll with 5s timeout for the completion signal
5. Assert: message arrived, elapsed time ≥ 40ms (allow 10ms undershoot tolerance)
6. Assert: `AlarmHandle` returned by `schedule()` is valid (non-zero)

### test_cancel_schedule_prevents_delivery

1. Create `ActorSystem` with `scheduler_threads = 2`
2. Spawn a test actor that schedules a 200ms message, then immediately cancels it
3. Wait 500ms and assert the message was never received
4. Verify `cancel_schedule(AlarmHandle{})` is a harmless no-op

### test_payment_timeout_scenario (integration)

1. Build and run `./13_order_platform --all-in-one --scenario payment-timeout`
2. Assert stdout contains `status=payment_timed_out`
