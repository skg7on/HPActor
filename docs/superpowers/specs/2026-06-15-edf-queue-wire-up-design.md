# EDF Queue Wire-Up — Design

Date: 2026-06-15
Issue: [#305](https://github.com/skg7on/HPActor/issues/305)

## Motivation

`EDFQueue` (Earliest Deadline First min-heap) is fully implemented in
`include/hpactor/sched/edf_queue.hpp` and owned by every `WorkerState`
(`work_placement_scheduler.hpp:216`). The consumer side is wired — `pop_local()`,
`try_steal()`, and `edf_next_deadline()` all read from it. But **zero code paths
call `edf_queue.push()`**. The queue is always empty.

The gap is in `WorkPlacementScheduler::enqueue_shared()`: it CAS-pushes work
into a per-worker lock-free `SharedInputNode` stack, and when the owning worker
drains it in `pop_local()`, items are distributed into `worker.queues[priority]`
(ChaseLev deques). The `WorkItem::deadline_ns` is carried through but never used
for ordering — items always land in priority-only ChaseLev deques.

This has two downstream effects:

1. **No EDF ordering at dispatch** — an actor woken by a 1ms deadline timer
   competes equally with an actor woken by an unbounded background message.
2. **EDF-aware CV timeout is no-op** — `enter_cv_block()` calls
   `edf_next_deadline()` to compute a CV timeout that wakes the worker before
   the earliest deadline expires. Since EDF is always empty, it falls back to
   a flat 100ms timeout.

## Design

### Principle: Opt-in EDF, preserve zero-overhead for priority-only

EDF is not a universal queue. The design constraint is:

> Only explicitly-deadlined messages enqueued through a dedicated API may enter
> the EDF queue. Ordinary priority-only messages (`deadline_ns = INT64_MAX`)
> stay on the ChaseLev deques — no change to the fast path.

This keeps the EDF heap small, avoids polluting it with every message in the
system, and makes the scheduling class explicit in the API.

### API Surface

A new `DeliveryOptions` flag gates EDF placement:

```cpp
// enqueue_result.hpp — DeliveryOptions
struct DeliveryOptions {
    // ... existing fields ...
    bool schedule_edf = false;  ///< If true, the work item is placed in the
                                ///< worker's EDFQueue instead of the priority
                                ///< ChaseLev deque. Requires deadline_ns !=
                                ///< INT64_MAX.
};
```

A new `ActorSystem` overload provides the public entry point:

```cpp
// actor_system.hpp
void deliver_local_edf(ActorId target, TypedMessage msg,
                       int64_t deadline_ns, uint8_t priority = 0);
```

Implementation:

```cpp
// actor_system.cpp
void ActorSystem::deliver_local_edf(ActorId target, TypedMessage msg,
                                     int64_t deadline_ns, uint8_t priority) {
    mailbox::DeliveryOptions options;
    options.schedule_edf = true;
    (void)delivery_pipeline_->try_deliver(target, std::move(msg), priority,
                                          deadline_ns, options);
}
```

A corresponding `ActorContext` method exposes it from within an actor:

```cpp
// actor_context.hpp
void send_edf(ActorAddress addr, TypedMessage msg,
              std::chrono::nanoseconds deadline, uint8_t priority = 0);
```

The existing `deliver_local()` overloads are unchanged. `context()->schedule()`
continues to use the timer-thread path for self-delivery — it does not use EDF
because the timer already provides deadline-gated wakeup.

### Routing Decision

The decision is made in `WorkPlacementScheduler::enqueue_shared()`. Today it
pushes every item into the shared-input stack. With this change, EDF-flagged
items bypass the shared-input stack and go directly into the worker's EDFQueue:

```
enqueue_shared(item, priority, worker_id)
    │
    ├─ item is EDF-flagged?
    │   YES → worker.edf_queue.push(item.deadline_ns, item)
    │         worker.wake_if_blocking()
    │         return
    │
    └─ NO  → (existing path) CAS-push to shared_input stack
             worker.wake_if_blocking()
```

**Why bypass the shared-input stack for EDF items?** The shared-input stack is
drained by the owning worker in LIFO order and reversed to approximate FIFO.
EDF items need to be ordered by deadline, not arrival time. Pushing directly
into the min-heap preserves the deadline ordering invariant without an
intermediate reorder step.

**Thread safety**: `EDFQueue` is not internally synchronized. Two producers
could call `push()` concurrently. We add a per-worker spin-lock for the EDF
queue, or accept that `enqueue_shared()` is called under a pre-existing
happens-before edge (the `SharedInputNode` CAS-release and the worker's
acquire on drain). Since the current design already relies on the CAS chain
for visibility, and `EDFQueue::push()` is a vector append + sift-up (no readers
during push), the simplest correct approach is a per-worker `edf_push_mutex_`.

**Alternative considered**: push EDF items through the shared-input stack and
sort them into `edf_queue` during `pop_local()` drain. Rejected because it adds
O(n log n) drain cost and delays deadline visibility for `edf_next_deadline()`.

### Consumer Path (no changes needed)

`pop_local()`, `try_steal()`, and `edf_next_deadline()` already read from
`worker.edf_queue` before the priority queues. Once items are pushed into EDF,
they are automatically ordered correctly:

```
pop_local(worker_id, out)
    │
    ├─ drain shared_input stack → reverse → worker.queues[prio].push_bottom()
    │
    ├─ pop_edf(worker_id, out)?   ← now returns true when EDF has items
    │   YES → return true
    │
    └─ for p in 0..N-1:
        worker.queues[p].pop_bottom(out)?
        YES → return true
```

The same applies to `try_steal()`:

```
try_steal(thief_id, out)
    │
    ├─ for each victim:
    │   ├─ victim.edf_queue.pop(out)?   ← now succeeds for EDF items
    │   │   YES → emit steal metric, return true
    │   └─ for p in 0..N-1:
    │       victim.queues[p].steal_top(out)?
    │       YES → emit steal metric, return true
```

### EDF-Aware CV Timeout (becomes functional)

`HybridScheduler::edf_next_deadline()` already peeks all workers' EDF queues.
Once populated, the CV timeout in `enter_cv_block()` becomes precise:

```cpp
// worker_thread.cpp:240-254 — today always falls back to 100ms
int64_t edf_ns = owner_->edf_next_deadline();
if (edf_ns != INT64_MAX) {
    int64_t delta_ns = edf_ns - now_ns;
    // Wake 1ms before deadline for steal + dispatch headroom
    timeout = max(1ms, delta_ns - 1ms);
    timeout = min(timeout, 100ms);
}
```

### Requeue Path

When an actor's `run()` returns `ActorRunDisposition::RequeueReady` (coroutine
yield or behavior-driven requeue), the item is re-enqueued via
`enqueue_admitted()`. The `WorkItem` already carries `deadline_ns` and
`sequence`. If the original item was EDF-scheduled, the requeued item should
also go to EDF. To support this, add an `edf_scheduled` flag to `WorkItem`:

```cpp
struct WorkItem {
    ActorId actor;
    int64_t deadline_ns;
    uint64_t sequence;
    bool edf_scheduled = false;  ///< True if this item was originally EDF-placed.
};
```

The flag is set in `notify_ready()` when the caller opts into EDF, and
`enqueue_admitted()` checks it to route back into EDF.

### End-to-End Flow

```
Actor A                              Scheduler
────────                              ─────────
context()->send_edf(B, msg, 5ms)
  └─ ActorSystem::deliver_local_edf()
       └─ DeliveryPipeline::try_deliver(..., options.schedule_edf=true)
            └─ mailbox->try_push(msg)
                 └─ enqueue_reserved()
                      └─ CAS mailbox_was_empty_ true→false
                           └─ scheduler_->notify_ready(B, priority, deadline_ns)
                                └─ WorkItem{B, deadline_ns, 0, edf_scheduled=true}
                                     └─ enqueue_admitted(item, priority)
                                          └─ enqueue_shared(item, priority, w)
                                               └─ worker.edf_queue.push(deadline_ns, item)
                                                    └─ worker.wake_if_blocking()
                                                         │
  Worker thread wakes ◄──────────────────────────────────┘
    └─ pop_local()
         └─ pop_edf() → WorkItem{B, deadline_ns=5ms}  ← earliest deadline
              └─ execute_actor(B)
```

## Thread Safety

| Operation | Contention | Mechanism |
|-----------|-----------|-----------|
| `edf_queue.push()` | Multi-producer (any thread calling `notify_ready`) | Per-worker `std::mutex edf_push_mutex_` |
| `edf_queue.pop()` | Single-consumer (owning worker only) | No lock needed |
| `edf_queue.peek()` | Read by `edf_next_deadline()` from timer or other worker | `std::mutex edf_push_mutex_` shared with push |
| `edf_queue.pop()` in `try_steal()` | Thief worker reads victim's EDF | Share `edf_push_mutex_` with victim's push |

Since EDF items are infrequent by design (only explicitly-deadlined messages),
the mutex contention is negligible. The mutex is per-worker, so the common case
(zero EDF items) has no contention at all.

**Alternative considered**: lock-free EDFQueue using a CAS-based heap.
Rejected for initial implementation — the per-worker mutex is simpler and the
EDF path is explicitly low-throughput. A lock-free upgrade can follow if
profiling shows contention.

## Deadlines and the Timer Thread

EDF dispatch is complementary to the timer thread, not a replacement:

| Mechanism | When used | Purpose |
|-----------|-----------|---------|
| Timer thread | `context()->schedule(delay, msg)` | Fires a callback after `delay` that calls `deliver_local()` |
| EDF queue | `context()->send_edf(addr, msg, deadline)` | Places the actor in EDF order so the worker dispatches by deadline |

The timer thread handles "deliver this message after 5ms" (fire-and-forget
delay). EDF handles "this actor has deadline-sensitive work — schedule it
before other actors with later deadlines." They serve different scheduling
granularities: timer = message-level, EDF = actor-dispatch-level.

When a timer callback fires and calls `deliver_local()`, the resulting
`notify_ready()` uses `deadline_ns=INT64_MAX` → priority queue (no EDF).
This is correct: the timer already provided the delay; the actor just needs
to run promptly. If the caller wants EDF ordering for the dispatched actor,
they use `deliver_local_edf()` instead of `schedule()` + `deliver_local()`.

## Files Changed

| File | Change |
|------|--------|
| `include/hpactor/sched/work_queue.hpp` | Add `bool edf_scheduled` to `WorkItem` |
| `include/hpactor/msg/enqueue_result.hpp` | Add `bool schedule_edf` to `DeliveryOptions` |
| `include/hpactor/sched/work_placement_scheduler.hpp` | Add `std::mutex edf_push_mutex_` to `WorkerState`; declare `enqueue_edf()` |
| `src/sched/work_placement_scheduler.cpp` | `enqueue_shared()`: route EDF-flagged items to `worker.edf_queue.push()`; add `enqueue_edf()` helper |
| `include/hpactor/sched/scheduler.hpp` | Declare `notify_ready_edf()` overload |
| `src/sched/scheduler.cpp` | `notify_ready()` sets `edf_scheduled` from options; `enqueue_admitted()` checks flag for requeue path |
| `include/hpactor/core/actor_system.hpp` | Declare `deliver_local_edf()` |
| `src/actor/actor_system.cpp` | Implement `deliver_local_edf()` |
| `include/hpactor/actor/actor_context.hpp` | Declare `send_edf()` |
| `src/actor/actor_context.cpp` | Implement `send_edf()` |

10 files, ~80 lines.

## Testing Strategy

### Unit Tests

| Test | What it validates |
|------|-------------------|
| `EDFQueue.push/pop ordering` | Existing test — confirms EDFQueue min-heap works correctly (already passes) |
| `enqueue_shared EDF routing` | When `schedule_edf=true`, item lands in `edf_queue` not `shared_input` |
| `enqueue_shared priority routing` | When `schedule_edf=false`, item lands in `shared_input` (no regression) |
| `pop_local EDF-first` | Worker drains EDF item before priority items when both are populated |
| `try_steal EDF-first` | Thief steals from EDF before priority queues |
| `requeue preserves edf_scheduled` | `RequeueReady` items with `edf_scheduled=true` return to EDF |
| `edf_next_deadline returns correct value` | Populated EDF → `edf_next_deadline()` returns earliest deadline |

### Integration Tests

| Test | What it validates |
|------|-------------------|
| `deliver_local_edf end-to-end` | Actor B receives message, worker dispatches via EDF path |
| `EDF ordering across actors` | Two actors with different deadlines — earlier deadline dispatched first |
| `send_edf from ActorContext` | `context()->send_edf()` produces correct EDF placement |
| `EDF + priority interleaving` | EDF item with late deadline dispatched after high-priority item with no deadline |

### Deterministic Testing

All scheduler tests use `scheduler_threads=0` and the worker-control API
(`pause_workers`, `pin_actor_to_worker`, `run_actor`). Push EDF items via
`inject_for_test`-style helpers, then use `run_one_on_worker` to verify
dispatch order. No timing-based assertions.

## Non-Goals

- **EDF for all messages** — priority-only messages stay on ChaseLev deques.
- **EDF-aware timer cancellation** — timers continue to use `TimingWheel` /
  `CalendarQueue` backends independently.
- **Lock-free EDFQueue** — the per-worker mutex is adequate for the opt-in,
  low-throughput EDF path. Evaluate lock-free upgrade if profiling shows
  contention.
- **EDF in dedicated-thread/pool actors** — dedicated execution contexts
  bypass the shared placement layer entirely. EDF is only for cooperative
  actors on the work-stealing scheduler.
