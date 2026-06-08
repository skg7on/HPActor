---
name: hpactor-scheduler-thread-safety
description: Use when modifying, reviewing, or testing HPActor scheduler, work stealing (A2WS), ready-state, worker placement, actor execution, mailbox, MultiLaneQueue, MPSC queue, Chase-Lev deque, passivation, coroutine execution, or multi-threaded actor delivery code.
---

# HPActor Scheduler Thread Safety

Use this skill before touching scheduler or lock-free queue code. The first job
is to state which thread owns each operation. If ownership cannot be stated
clearly, stop and inspect the implementation before changing it.

## Required Reads (Conditional)

Always read `AGENTS.md` and `CLAUDE.md` for repo conventions.

Read these only when they apply to the subsystem under change:

- **Any scheduler, mailbox, or actor-concurrency change:**
  `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- **Work stealing or worker queue changes:**
  `include/hpactor/sched/a2ws.hpp`,
  `include/hpactor/adt/chaselev_deque.hpp`,
  `include/hpactor/adt/multi_priority_work_queue.hpp`
- **Scheduler readiness or execution changes:**
  `include/hpactor/sched/actor_ready_gate.hpp`,
  `include/hpactor/sched/actor_execution_engine.hpp`,
  `include/hpactor/sched/scheduler_interfaces.hpp`
- **Worker thread changes:**
  `include/hpactor/sched/worker_thread.hpp`,
  `src/sched/worker_thread.cpp`
- **Mailbox or delivery changes:**
  `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`,
  `include/hpactor/mailbox/multi_lane_queue.hpp`
- **Passivation or lifecycle changes:**
  `include/hpactor/actor/lifecycle/passivation_manager.hpp`,
  `include/hpactor/actor/actor_route.hpp`

Work in the required `.worktrees/` checkout before writing designs, docs, code,
tests, or build files.

## Core Contracts

### Chase-Lev and MultiPriorityWorkQueue

- `ChaselevDeque::push_bottom()` and `pop_bottom()` are single-owner
  operations. Only the owning worker thread may call them.
- `ChaselevDeque::steal_top()` is the multi-thief operation. Concurrent thief
  threads may call it.
- `MultiPriorityWorkQueue::push()` and `pop()` inherit the single-owner bottom
  contract. `steal()` inherits the multi-thief top contract.
- Cross-thread submissions must not call `push_bottom()` on a worker deque.
  Route them through `WorkPlacementScheduler::enqueue_admitted()`, which
  CAS-pushes to the worker's `shared_input` stack. The owning worker drains
  that stack in `pop_local()` and then pushes into its Chase-Lev deque safely.
- `size_approx()` and `depth_approx()` are relaxed snapshots. Do not use them
  as linearizable guards under concurrent mutation.
- Deque growth and `garbage_` updates happen on the owner push path. Do not add
  non-owner resizing, reset, or destruction paths; destruction requires all
  worker and thief activity to be quiesced.
- Do not weaken acquire/release/CAS ordering without a written concurrency
  proof plus stress or model-checker tests.

### A2WS: Adaptive Two-Level Work Stealing

The A2WS class (`include/hpactor/sched/a2ws.hpp`) implements victim selection
across worker pools. Its contracts:

- `get_victim(thief_index)` — called by any thief worker concurrently.
  Must be lock-free and allocation-free (hot path when work is scarce).
  Reads `victim_hints_` which are `std::atomic<uint32_t>`. The current
  implementation is O(1) round-robin. Any change to scan-based or
  preference-based selection must remain bounded (use `victim_scan_limit`).
- `record_attempt(thief, victim, success)` and `record_steal(thief, victim)`
  — called by any thief concurrently. Write to per-worker `WorkerStats` which
  uses `std::atomic<uint64_t>` — safe for concurrent `fetch_add`.
- `WorkerStats` fields (`steal_attempts`, `steal_successes`, `local_steals`,
  `remote_steals`) are relaxed atomics. They are observability hints, not
  linearizable correctness guards. Do not use them to prove work was or was
  not available.
- `victim_hints_` — per-thief atomic suggestions. A thief updates its own
  hint after each attempt. No thief writes another thief's hint.
- Victim pool ranges (`get_victim_pool()`) are computed from static
  configuration. They are immutable after construction.

### WorkerThread Contracts

`WorkerThread` (`include/hpactor/sched/worker_thread.hpp`) owns one OS thread
and one `MultiPriorityWorkQueue`. Key contracts:

- `push(priority, item)` — owner operation. Only the owning worker or
  `WorkPlacementScheduler::enqueue_admitted()` (via `shared_input` drain)
  may push.
- `pop(out)` — owner operation. Only the owning worker may pop from its
  own queue.
- `steal(out)` — thief operation. Any other worker may call. The owning
  worker must not call `steal()` on its own queue.
- `try_steal(out)` — thief operation using A2WS victim selection. Calls
  `A2WS::get_victim()` then attempts `steal()` on the selected victim.
- `donation_count()` — relaxed atomic snapshot. Used by A2WS for victim
  preference heuristics. Not a guarantee of available work.
- `pause_handler_` — test hook set by the scheduler to block workers during
  deterministic tests. Production code must not depend on it.
- `allocator_` — per-thread slab allocator. Not thread-safe across workers.
  `enable_thread_allocator` is `false` for scheduler workers.
- `frame_pool_` — per-worker coroutine frame pool. Frame acquisition and
  release are owner operations (same worker only).

### Narrow Scheduler Interfaces (for Testability)

`scheduler_interfaces.hpp` defines three narrow interfaces that decouple
subsystems from the full scheduler. Use these in tests to mock scheduler
behavior:

- `IActorReadyNotifier::notify_ready(actor, priority, deadline_ns)` — called
  by mailboxes, awaiters, and timer callbacks. Thread-safe from any thread.
  Mock this in mailbox/awaiter unit tests to observe readiness notifications
  without a real scheduler.
- `ITimerService::schedule_after(cb, delay_ns)` / `cancel_timer(handle)` —
  called by `TimerAwaiter` and timer-dependent code. Thread-safe from any
  thread. Mock this to control timer firing in tests.
- `IActorYieldScheduler::yield(actor, priority)` — called by `YieldAwaiter`
  from within the actor's own execution context (the worker thread running
  the actor). Must NOT be called from other threads.

### Scheduler Readiness and Execution

- Public readiness notifications go through
  `ActorReadyGate::try_mark_ready()`, normally via `HybridScheduler::notify_ready()`.
  It admits only `Idle` or `IOWaiting` event-based actors.
- Owned requeues go through
  `ActorReadyGate::mark_ready_already_admitted()`. Use it only when the caller
  already owns the activation, such as after an execution slice or yield.
- A `WorkItem` does not grant execution rights. The runner must win the
  `Ready -> Running` CAS before calling actor code.
- If `Ready -> Running` CAS fails, do not recover by calling `receive()`.
  Another state transition won.
- Do not set `ActorState` directly in production feature code unless the code
  is actor lifecycle, ready gate, execution engine, or a documented actor-mode
  transition.
- Do not call `receive()` directly from sender, timer, event-loop,
  backpressure, test producer, or placement code. Actor execution belongs in
  `ActorExecutionEngine` and narrowly scoped test doubles.
- `WorkPlacementScheduler` moves `WorkItem` values only. It must not inspect or
  mutate actor internals.
- Dedicated-thread actors are suppressed from shared cooperative placement.
  Do not enqueue them onto shared queues unless the design explicitly changes
  that contract.
- Scheduler workers currently set `WorkerThread::Config::enable_thread_allocator`
  to `false` because slab caches are not thread-safe across scheduler workers.
  Do not re-enable that allocator on scheduler workers without proving the
  allocator ownership model.

### ActorExecutionEngine Contracts

`ActorExecutionEngine` (`include/hpactor/sched/actor_execution_engine.hpp`)
owns the activation decision after a worker selects a `WorkItem`. It dispatches
to `BehaviorActorRunner` or `CoroutineActorRunner`:

- `ActorRunDisposition` defines the outcomes: `Skipped` (wrong state),
  `SuspendedOrIdle` (actor done for now), `RequeueReady` (mailbox still has
  work — caller must requeue via `mark_ready_already_admitted()`),
  `Terminated` (actor reached terminal state, `on_exit()` was called).
- `BehaviorActorRunner::run()` pops at most one message, checks deadline
  expiry, calls `actor.receive()`, and performs the lost-wakeup double-check
  before returning `SuspendedOrIdle`.
- `RequeueReady` means the actor's mailbox still had messages after processing
  one. The caller must call `mark_ready_already_admitted()` to requeue.
- `CoroutineActorRunner::run()` lazily starts the coroutine on first
  invocation, resumes it, and observes the post-resume state. It returns
  `SuspendedOrIdle` if the coroutine suspended, `Terminated` if it completed.
- At most one caller may invoke `run()` per actor at a time. The
  `Ready -> Running` CAS enforces this.

### Mailbox and Multi-Lane Queues

- MPSC queues are many-producer, exactly-one-serialized-consumer structures.
  More consumer paths require external serialization.
- `MultiLaneQueue` has concurrent producer enqueue, but no built-in consumer
  lock. `dequeue()`, lane drops, reset, and pending-free operations require
  external serialization.
- `MPSCActorMailbox` owns consumer serialization via `consumer_lock_`,
  reservation accounting, overflow callbacks, edge-triggered wakeup, metrics,
  logging, and deferred destruction. Add mailbox behavior there or in
  mailbox-owned helpers — all consumer operations must acquire `consumer_lock_`.
- Do not hold `consumer_lock_` across actor `receive()`, scheduler calls, user
  callbacks, blocking I/O, logging sinks that may block, or remote transport
  calls. Drain into a temporary container, release the lock, then process.
- Do not add a second mailbox/lane consumer for speed. It violates both the
  MPSC contract and the actor model.
- Preserve the quiet-period wakeup invariant: the first enqueue after empty
  notifies the scheduler; later enqueues in the same non-empty period must not
  create duplicate runnable entries. After any drain or eviction operation that
  empties the mailbox, reset `mailbox_was_empty_` to true.
- User-facing delivery should enter through `ActorContext` or
  `ActorSystem::try_deliver_local()`. Direct mailbox calls bypass TTL,
  deduplication, DLQ, tracing, backpressure, quarantine, and failure envelopes.
- Reservation accounting must stay paired with every user-message dequeue,
  drop, spill, eviction, or drain. Use mailbox-owned helpers; don't write
  separate accounting.

### Passivation and Scheduler Interaction

Passivation removes an actor from the scheduler while preserving its identity.
The architecture doc (`actor-concurrency-and-lockfree-mailbox-rules.md`) has
these passivation-specific scheduler rules:

1. A passivated actor (`LifecycleState::kPassivated`) has no live actor object
   and no scheduler work items. The `LocalPassivatedRoute` stub owns the
   lifecycle state in the registry. During passivation, no scheduler thread may
   call `receive()` on the passivated actor.
2. Reactivation is triggered by the first message arriving at
   `LocalPassivatedRoute::try_deliver()`. The triggering thread (a producer)
   sets `reactivation_in_progress` and spawns a reactivation task. Subsequent
   producers see the flag already set and only enqueue to the bounded
   reactivation buffer.
3. The reactivation task is the single consumer of the reactivation buffer.
   It runs on a dedicated thread pool — NOT a scheduler worker.
4. The reactivation buffer is a bounded MPSC queue. Producers may enqueue
   concurrently. If the buffer fills before reactivation completes,
   `try_deliver()` returns `EnqueueResult::Rejected` with
   `FailureReason::PassivationQueueFull`.
5. Passivation state transitions (`Active -> Passivating -> Passivated`,
   `Passivated -> Recovering -> Active`) are validated through the same
   constexpr transition table and CAS as all other lifecycle transitions.
   The route stub owns the lifecycle state during `kPassivated` — there is
   no actor object to CAS against, so the stub performs the CAS directly
   on the stored atomic state.
6. Before passivation completes, all in-flight scheduler work items for the
   actor must be drained or cancelled. After passivation, no new work items
   may exist for the old actor.

## Design Checklist

Before implementation, write down:

1. The owning thread or component for every queue operation touched.
2. Which operations are multi-producer, single-owner, thief-only, or externally
   serialized.
3. The actor state transitions and the CAS that admits each transition.
4. Where nodes or messages are allocated, who owns them, and who destroys them.
5. Where bounded capacity is reserved and released.
6. Whether delivery metadata is preserved: sender, trace, priority, deadline,
   message id, flags, and failure reason.
7. What happens on rejection, overflow, expiration, actor-not-found,
   mailbox-closed, circuit-open, draining, stopping, terminated, passivated,
   and passivation-queue-full paths.
8. What callbacks run on scheduler workers, timer threads, event-loop threads,
   dedicated actor threads, producer threads, reactivation threads, or test
   threads.

## Testing Rules

- Prefer paused workers and deterministic scheduler controls:
  `scheduler_threads = 0`, `scheduler_start_paused = true`, `run_one_ready()`,
  `drain_ready()`, `pin_actor_to_worker()`, `run_actor()`, and
  `run_one_on_worker()`.
- Use the narrow scheduler interfaces (`IActorReadyNotifier`, `ITimerService`,
  `IActorYieldScheduler`) to mock scheduler behavior in unit tests. Implement
  stub versions that record calls without real threads.
- Avoid sleeps as proof of scheduler progress. Use controlled execution or
  condition-based polling with generous timeouts.
- Assert exact ordering only within one queue lane unless the test is
  explicitly about system-first, priority-lane, EDF, or steal ordering.
- For scheduler and lock-free changes, prove no duplicate execution, no lost
  wakeup, bounded memory, and correct owner/thief behavior.
- Add stress, race-oriented, sanitizer, or model tests when changing
  `ChaselevDeque`, `MultiPriorityWorkQueue`, `WorkPlacementScheduler`,
  `ActorReadyGate`, `ActorExecutionEngine`, `MPSCActorMailbox`,
  `MultiLaneQueue`, `A2WS`, timers, passivation, or transport callback
  delivery.
- For passivation tests: use `scheduler_threads = 0` to control execution
  timing, verify exact lifecycle state transitions, and assert reactivation
  buffer capacity limits.

## Anti-Patterns

- Cross-thread `push_bottom()` into a worker deque.
- Multiple consumers for an MPSC queue or `MultiLaneQueue` without external
  serialization.
- Direct `receive()` outside the execution engine.
- Direct actor-state mutation from delivery, placement, timer, or callback code.
- Treating approximate depths, snapshots, `empty()`, `count()`, or
  `donation_count()` as stable facts while producers or consumers are active.
- Holding `consumer_lock_` across blocking operations (I/O, logging, actor
  callbacks, CLI output).
- Dropping or evicting messages without matching reservation release,
  destruction, metrics/logging, and DLQ/backpressure visibility.
- Adding unbounded queues, vectors, maps, or blocking I/O to actor hot paths.
- Weakening atomic ordering because a local test appears to pass.
- Calling `receive()` or enqueuing work items for a passivated actor.
- Running reactivation work on a scheduler worker thread.
- Using `get_victim()` with an unbounded scan — it's on the thief hot path.
- Adding a drain or inspection path that skips `consumer_lock_` because "it's
  just reading."
