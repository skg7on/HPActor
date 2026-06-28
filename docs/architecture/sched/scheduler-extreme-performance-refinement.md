# Scheduler Extreme-Performance Refinement Design

**Date:** 2026-06-28  
**Author:** HPActor Engineering  
**Status:** Draft — Pending review and TDDFlow implementation

---

## 1. Context and Goal

HPActor's HybridScheduler already implements the right architectural skeleton
(activation-based work stealing, per-actor MPSC mailboxes, Chase-Lev deques,
A2WS victim selection, OS-calibrated adaptive backoff). This document identifies
the remaining performance gaps relative to the brainstorm principles, proposes
concrete refinements, and sets acceptance targets to outperform CAF on the
bench_caf scenarios.

**Target:** HPActor message throughput ≥ 1.5× CAF on all bench_caf scenarios
(ping-pong, fan-in, fan-out, ring, pipeline) at equal worker count.

---

## 2. What HPActor Already Gets Right

| Principle | Status | Evidence |
|---|---|---|
| Activation-based (actor enqueued once) | ✅ | `ActorReadyGate` CAS prevents double-scheduling |
| Per-actor MPSC mailbox | ✅ | `MPSCActorMailbox` with edge-triggered CAS wakeup |
| Chase-Lev work stealing | ✅ | `ChaselevDeque` per priority, `A2WS` victim selection |
| OS-calibrated idle backoff | ✅ | `BackoffCalibration` (yield → proportional sleep → CV) |
| Per-worker inject queue | ✅ | `SharedInputNode*` CAS-push stack |
| Bounded batch execution | ✅ | Sequence counter with yield at 128 |
| Batch send | ✅ | `try_push_batch()` single reservation + single wakeup CAS |
| EDF deadline scheduling | ✅ | Per-worker `EDFQueue` + `notify_ready_edf()` |

The foundation is correct. The gaps are all micro-architecture issues.

---

## 3. Performance Gaps (Priority-Ordered)

### Gap 1: `system_.get_actor(ActorId)` on every dispatch [CRITICAL]

**Current:** `execute_actor()` calls `system_.get_actor(item.actor)` — a hash map
lookup — on every single actor activation. This is the innermost hot-path step.

**Cost:** ~20–40 ns per dispatch (hash probe, possible cache miss into
`actor_registry_` map).

**Fix:** Store `EventBasedActor*` directly in `WorkItem`. Actor pointers are
stable for the actor's lifetime (HPActor does not move live actors). The pointer
becomes stale only at actor termination, which is already guarded by the
`ActorState::Terminated` check inside `BehaviorActorRunner::run()`.

```cpp
struct WorkItem {
    EventBasedActor* actor_ptr;  // direct pointer, no lookup
    ActorId          actor;      // retained for metrics/log attribution
    int64_t          deadline_ns;
    uint64_t         sequence;
    bool             edf_scheduled = false;
};
```

`WorkPlacementScheduler::enqueue_admitted()` receives the pointer from the
caller; callers already hold it (mailbox wakeup path has the actor pointer).

**Expected gain:** 20–40 ns per dispatch eliminated → ~15–25% throughput
improvement on ping-pong where dispatch is the bottleneck.

---

### Gap 2: No Home-Worker Affinity [HIGH]

**Current:** `choose_worker()` in `WorkPlacementScheduler` uses a modulo hash
over the actor ID but does not track which worker last executed the actor.
Hot actors' state (behavior handler vtable, mailbox head, stack frame) is
evicted from L1/L2 whenever the actor migrates to a different core.

**Fix:** Add `home_worker_id` (uint16_t) to `EventBasedActor` / `LocalActor`.
Set at spawn time:

```cpp
actor.home_worker_id = hash(actor.id()) % scheduler.worker_count();
```

In `enqueue_admitted()`, route to the home worker's inject queue first.
Work stealing remains unchanged — a thief can still steal the actor. After a
steal, update `home_worker_id` to the thief (locality follows execution).

This matches the brainstorm's `actor.home_worker = hash(actor_id) % worker_count`
principle exactly.

**Expected gain:** 10–20% on latency-sensitive scenarios (ping-pong, ring)
where the same pair of actors exchanges messages in tight loops.

---

### Gap 3: `SharedInputNode` Heap Allocation on Inject Path [HIGH]

**Current:** Cross-thread work injection uses a `SharedInputNode*` intrusive
linked list. Each injection allocates a `SharedInputNode` from the heap and
frees it when the owning worker drains the stack. On the ping-pong hot path,
this is one `mem::allocate` + one `mem::deallocate` per scheduling wakeup.

**Fix:** Replace the linked-list stack with a per-worker bounded **MPSC inject
ring** (lock-free, allocation-free):

```
struct alignas(64) InjectSlot {
    std::atomic<uint64_t> seq;
    WorkItem             item;   // ~32 bytes
};

struct alignas(64) WorkerInjectRing {
    InjectSlot  slots[256];      // 256 × 64B = 16 KB (L1 resident)
    uint64_t    mask{255};
    alignas(64) std::atomic<uint64_t> tail{0};  // producers
    alignas(64) uint64_t            head{0};    // owner only
};
```

Producers `fetch_add(tail)` to claim a slot, spin briefly waiting for `seq ==
pos`, write the item, then set `seq = pos + 1`. Owner drains by reading `seq ==
head + 1`. No allocation, no pointer chasing, cache-friendly sequential access.

For 256 slots, this ring is 16 KB per worker — fits in L1 on most modern CPUs
(32–64 KB L1). With 16 workers total ring memory is 256 KB.

**Expected gain:** Eliminates malloc/free on the inject path, which is ~30–50 ns
per ping-pong round trip.

---

### Gap 4: Condition Variable Wakeup Overhead [HIGH]

**Current:** Worker sleep uses `WorkerState::sleep_cv_` + `sleep_mutex_`. The
producer-side wakeup path acquires `lock_guard<mutex>(sleep_mutex_)` even for
the common fast path where `is_blocking_ == false`. On Linux this is two
`pthread_mutex_lock/unlock` calls per wakeup.

**Brainstorm recommendation:** EventCount / Futex-based parking:

```text
seq.fetch_add(1)          // signal new work
futex_wake_one(&seq)      // syscall only when worker is actually sleeping
```

On Linux, `futex(FUTEX_WAKE, 1)` is a single syscall that bypasses the mutex
entirely. A sleeping worker issues `futex(FUTEX_WAIT, expected_seq)`.

**Fix:** Introduce `WorkerPark` replacing `WorkerState::sleep_cv_`:

```cpp
struct alignas(64) WorkerPark {
    std::atomic<uint32_t> seq{0};       // incremented by any producer
    std::atomic<bool>     parked{false};
    // Platform impl: futex (Linux), ulock (macOS), CV (fallback)
};
```

Producer path (called from `enqueue_shared()`):
1. `seq.fetch_add(1, release)` — increment first so worker sees new seq.
2. If `parked.load(acquire)` is true: issue `futex_wake_one` (Linux) or
   `os_unfair_lock_signal` (macOS). No mutex.

Worker park:
1. Capture `uint32_t snap = seq.load(acquire)`.
2. Publish `parked.store(true, seq_cst)`.
3. Re-check work — if found, `parked.store(false)`, return.
4. `futex_wait(&seq, snap, timeout_ns)` — sleeps only if seq unchanged.
5. `parked.store(false)`.

The seq_cst store in step 2 + the seq_cst fence from the producer's
`fetch_add(release)` + `parked` load ensures no lost-wakeup races.

macOS: Use `__ulock_wait`/`__ulock_wake` (available since macOS 10.12,
no public header, called via `syscall()`). Fall back to `std::condition_variable`
when neither is available.

**Expected gain:** ~50–100 ns per wakeup event eliminated → measurable on
low-concurrency ping-pong where wakeup is on the critical path.

---

### Gap 5: Fixed Batch Cap Without Adaptive Continuation [MEDIUM]

**Current:** `execute_actor()` requeues the actor when the mailbox has remaining
work, using a sequence counter to force a yield at 128 re-executions. This
always requeues even when the worker's local queue is empty, paying the full
`enqueue_admitted()` + `notify_ready()` round trip needlessly.

**Fix:** Adaptive batch continuation — after each message, check local queue
depth. If empty, continue processing the same actor in-place (no requeue):

In `BehaviorActorRunner::run()`, change the disposition from `RequeueReady` to
a new `ContinueSameActor` disposition when:
- The mailbox has remaining messages, AND
- The worker's local queue is empty, AND
- The time budget (`kBatchMaxNs = 20_000 ns`) has not expired.

`execute_actor()` handles `ContinueSameActor` with a tight local loop:

```cpp
while (result.disposition == ActorRunDisposition::ContinueSameActor &&
       !pop_local(next_item, worker_id) &&
       budget_remaining()) {
    result = executor_.run(*actor, item, ctx, use_coroutines());
}
if (result.disposition == ActorRunDisposition::RequeueReady) {
    enqueue_admitted(next_requeue, result.priority);
}
```

This matches the brainstorm's §10 adaptive policy:
> If local queue is empty, continue draining the current Actor.
> If local queue is non-empty, requeue after batch expires.

**Expected gain:** 10–20% on fan-in scenarios where one receiver gets many
messages from many senders — the consumer actor runs continuously rather than
re-enqueueing itself 64 times per second.

---

### Gap 6: Multi-Priority Scan Overhead [MEDIUM]

**Current:** `MultiPriorityWorkQueue` has `num_priorities` Chase-Lev deques.
`pop()` scans from priority 0 downward. Even for single-priority workloads,
this is an array traversal + empty check per deque.

**Fix:** Add a single-priority fast path. When `num_priorities == 1` (the
default and most common case, including all bench_caf scenarios), bypass
`MultiPriorityWorkQueue` entirely and use a bare `ChaselevDeque<WorkItem>`:

```cpp
// WorkerThread: if only 1 priority level
if (config_.priority_levels == 1) {
    return single_queue_.pop(out);  // one deque, no scan
}
return priority_queue_.pop(out);   // existing path
```

Configurable at construction time. No behavioral change for multi-priority
workloads.

**Expected gain:** ~5 ns per pop eliminated on single-priority workloads.

---

### Gap 7: Per-Thread Allocator Disabled [MEDIUM]

**Current:** `WorkerThread::Config::enable_thread_allocator = false` in
`HybridScheduler::start()`. This means all `TypedMessage` allocations (via
`mem::allocate()` in `try_push()`) go through the global allocator, which
uses a mutex-protected segment pool under contention.

**Context:** The comment says "slab caches are not thread-safe". This referred
to the cross-thread `SlabCache` corruption that was fixed by introducing
`WorkerThread`. The fix (workers own their thread-local allocator) should make
it safe to re-enable.

**Fix:** Enable `enable_thread_allocator = true` in `HybridScheduler::start()`.
Validate with TSAN that no cross-thread slab corruption occurs. Add a dedicated
integration test that stress-tests per-thread allocation across worker lifetime.

**Expected gain:** Removes global allocator mutex contention under high
message-creation rate. Most significant on multi-core fan-out scenarios.

---

### Gap 8: `notify_ready` Path Lookup in `try_mark_yield_ready` [LOW]

**Current:** `try_mark_yield_ready()` calls `system_.get_actor()` to obtain the
`EventBasedActor*` before calling `ready_gate_.mark_ready_already_admitted()`.
This is only called on the yield path, not the critical enqueue path.

**Fix:** Pass the actor pointer directly through the yield API when available.
This is a minor cleanup, not a primary optimization.

---

## 4. Implementation Roadmap

| ID | Title | Gap # | Estimated Gain | Risk |
|---|---|---|---|---|
| SCHED-01 | Direct actor pointer in WorkItem | 1 | 15–25% dispatch | Low (additive field) |
| SCHED-02 | Home-worker affinity | 2 | 10–20% latency | Low (hash at spawn) |
| SCHED-03 | Bounded MPSC inject ring | 3 | 30–50 ns/roundtrip | Medium (new data structure) |
| SCHED-04 | Futex/EventCount worker park | 4 | 50–100 ns/wakeup | Medium (platform porting) |
| SCHED-05 | Adaptive batch continuation | 5 | 10–20% fan-in | Low (logic change only) |
| SCHED-06 | Single-priority fast path | 6 | ~5 ns/pop | Low (opt-in fast path) |
| SCHED-07 | Re-enable per-thread allocator | 7 | High on fan-out | Medium (TSAN validation needed) |

Recommended order: SCHED-01 → SCHED-02 → SCHED-05 → SCHED-06 → SCHED-03 →
SCHED-04 → SCHED-07. Start with zero-risk gains (01, 02, 05, 06), then tackle
the two data structure changes (03, 04) that require new tests, then re-enable
the allocator (07) gated on TSAN clean.

---

## 5. Architecture Diagram (Refined)

```
send(msg)
Producer ──────────────────► Actor.mailbox (MPSC)
                                    │
                                    │ IDLE→SCHEDULED CAS (ActorReadyGate)
                                    │ actor->home_worker_id → inject ring
                                    ▼
          ┌─────────────────────────────────────────────────┐
          │  Worker[home]                                   │
          │  ┌──────────────┐  ┌──────────────────────────┐│
          │  │ Local Deque  │  │ InjectRing (lock-free,   ││
          │  │ (Chase-Lev)  │◄─│  no-alloc, 256-slot)     ││
          │  └──────┬───────┘  └──────────────────────────┘│
          │         │                                       │
          │  1. pop local                                   │
          │  2. drain inject ring                           │
          │  3. A2WS steal from other workers              │
          │  4. Adaptive spin → EventCount park            │
          └─────────────────┬───────────────────────────────┘
                            │
                            ▼
                    execute_actor(actor_ptr)   ← no get_actor() lookup
                            │
                    batch loop (count ≤ 64, time ≤ 20µs)
                    if (mailbox non-empty && local empty)
                        → continue same actor (no requeue)
                    else
                        → requeue to local deque
```

---

## 6. Benchmark Acceptance Criteria

All bench_caf scenarios run with the same flags as CI. HPActor must beat CAF
at `--workers=auto` (logical CPU count):

| Scenario | Current HPActor vs CAF | Target after refinements |
|---|---|---|
| ping-pong (2 actors) | ~0.9× | ≥ 1.5× |
| ring (N actors) | ~1.1× | ≥ 1.6× |
| fan-in (N→1) | ~1.0× | ≥ 1.5× |
| fan-out (1→N) | ~0.95× | ≥ 1.5× |
| pipeline (N stages) | ~1.1× | ≥ 1.4× |

Each change is benchmarked in isolation (worktree branch per SCHED-XX) to
attribute gains. Run `apps/bench_caf` with `--format=json` and the report
generator for automated comparison.

---

## 7. Concurrency Contracts for Changed Components

### WorkItem with actor_ptr
- Pointer is valid from actor spawn until `ActorState::Terminated`.
- `execute_actor()` still checks `actor_ptr->is_event_based_actor()` as a
  type guard.
- Actor destruction (`~AbstractActor`) must guarantee no `WorkItem` referencing
  it exists in any worker queue. This is already ensured by the drain shutdown
  sequence (`ActorSystem::shutdown()` drains all workers before destroying actors).

### InjectRing
- Producers may spin up to 1 iteration waiting for a slot (very brief; ring
  capacity 256 > typical burst). If the ring is full (producer exhausted all
  256 slots before the worker drained), fall back to the existing SharedInputNode
  path as a slow-path safety valve.
- Owner drains by scanning from `head` upward. Owner increments head by N
  after draining N items and pushing them to the local deque.

### Home-Worker Affinity
- `home_worker_id` is a hint, not a hard constraint. The placement layer falls
  back to round-robin if the home worker's inject ring is full.
- After a steal, `home_worker_id` is updated to the thief worker ID to maintain
  locality for subsequent activations.

### EventCount Park
- Lost-wakeup protocol (section 4 above) must be tested with a dedicated
  stress test: N producers sending 1 message each to the same actor,
  verifying exactly N activations.
- `parked.store(true, seq_cst)` ensures the producer-side `seq.load(acquire)`
  observes the parked state before the producer's `fetch_add` completes.

---

## 8. Files Affected

| File | Change |
|---|---|
| `include/hpactor/sched/work_queue.hpp` | Add `actor_ptr` to `WorkItem` |
| `include/hpactor/sched/work_placement_scheduler.hpp` | `InjectRing` replaces `SharedInputNode`; `home_worker_id` routing |
| `include/hpactor/sched/worker_thread.hpp` | `WorkerPark` struct; single-priority fast path |
| `src/sched/scheduler.cpp` | Remove `get_actor()` in `execute_actor()`; adaptive batch loop |
| `src/sched/work_placement_scheduler.cpp` | Home-worker enqueue; inject ring drain |
| `src/sched/worker_thread.cpp` | `WorkerPark` park/unpark; platform `#ifdef` |
| `include/hpactor/actor/local_actor.hpp` | Add `home_worker_id` field |
| `src/actor/actor_system.cpp` | Set `home_worker_id` at spawn |
| `tests/unit/sched/test_inject_ring.cpp` | New: bounded MPSC inject ring |
| `tests/unit/sched/test_worker_park.cpp` | New: EventCount park/wakeup, lost-wakeup |
| `tests/unit/sched/test_home_worker.cpp` | New: affinity routing, post-steal update |
| `tests/integration/sched/test_adaptive_batch.cpp` | New: continuation vs requeue coverage |

---

## 9. What This Design Does NOT Change

- Actor API (`context()->send()`, `become()`, `co_await`, supervision) — unchanged.
- Mailbox semantics (MPSC, bounded, backpressure, DLQ) — unchanged.
- Timer backends (TimingWheel, CalendarQueue, TimerPlane) — unchanged.
- EDF scheduling — unchanged.
- Fault injection hook points — unchanged.
- Test harness (pause/resume workers, `scheduler_threads=0`) — unchanged.

All existing 2105+ tests must pass without modification.
