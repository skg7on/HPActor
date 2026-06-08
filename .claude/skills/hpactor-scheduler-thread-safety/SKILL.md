---
name: hpactor-scheduler-thread-safety
description: Use when modifying, reviewing, or testing HPActor scheduler, work stealing, ready-state, worker placement, actor execution, mailbox, MultiLaneQueue, MPSC queue, Chase-Lev deque, or multi-threaded actor delivery code.
---

# HPActor Scheduler Thread Safety

Use this skill before touching scheduler or lock-free queue code. The first job
is to state which thread owns each operation. If ownership cannot be stated
clearly, stop and inspect the implementation before changing it.

## Required Reads

Read the repo warmup files first: `AGENTS.md`, `CLAUDE.md`,
`CLAUDE_MEMORY.md`, and `HPACTOR_PROJECT_OUTLINE.md`.

For scheduler, mailbox, or actor-concurrency changes, also read:

- `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- `include/hpactor/adt/chaselev_deque.hpp`
- `include/hpactor/sched/scheduler.hpp`
- `include/hpactor/sched/work_placement_scheduler.hpp`
- `src/sched/work_placement_scheduler.cpp`
- `include/hpactor/sched/actor_ready_gate.hpp`
- `include/hpactor/sched/actor_execution_engine.hpp`

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

### Mailbox and Multi-Lane Queues

- MPSC queues are many-producer, exactly-one-serialized-consumer structures.
  More consumer paths require external serialization.
- `MultiLaneQueue` has concurrent producer enqueue, but no built-in consumer
  lock. `dequeue()`, lane drops, reset, and pending-free operations require
  external serialization.
- `MPSCActorMailbox` owns consumer serialization, reservation accounting,
  overflow callbacks, edge-triggered wakeup, metrics, logging, and deferred
  destruction. Add mailbox behavior there or in mailbox-owned helpers.
- Do not add a second mailbox/lane consumer for speed. It violates both the
  MPSC contract and the actor model.
- Preserve the quiet-period wakeup invariant: the first enqueue after empty
  notifies the scheduler; later enqueues in the same non-empty period must not
  create duplicate runnable entries.
- User-facing delivery should enter through `ActorContext` or
  `ActorSystem::try_deliver_local()`. Direct mailbox calls bypass TTL,
  deduplication, DLQ, tracing, backpressure, quarantine, and failure envelopes.

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
   mailbox-closed, circuit-open, draining, stopping, and terminated paths.
8. What callbacks run on scheduler workers, timer threads, event-loop threads,
   dedicated actor threads, producer threads, or test threads.

## Testing Rules

- Prefer paused workers and deterministic scheduler controls:
  `scheduler_threads = 0`, `scheduler_start_paused = true`, `run_one_ready()`,
  `drain_ready()`, `pin_actor_to_worker()`, `run_actor()`, and
  `run_one_on_worker()`.
- Avoid sleeps as proof of scheduler progress. Use controlled execution or
  condition-based polling with generous timeouts.
- Assert exact ordering only within one queue lane unless the test is
  explicitly about system-first, priority-lane, EDF, or steal ordering.
- For scheduler and lock-free changes, prove no duplicate execution, no lost
  wakeup, bounded memory, and correct owner/thief behavior.
- Add stress, race-oriented, sanitizer, or model tests when changing
  `ChaselevDeque`, `MultiPriorityWorkQueue`, `WorkPlacementScheduler`,
  `ActorReadyGate`, `ActorExecutionEngine`, `MPSCActorMailbox`,
  `MultiLaneQueue`, timers, or transport callback delivery.

## Anti-Patterns

- Cross-thread `push_bottom()` into a worker deque.
- Multiple consumers for an MPSC queue or `MultiLaneQueue` without external
  serialization.
- Direct `receive()` outside the execution engine.
- Direct actor-state mutation from delivery, placement, timer, or callback code.
- Treating approximate depths, snapshots, `empty()`, or `count()` as stable
  facts while producers or consumers are active.
- Dropping or evicting messages without matching reservation release,
  destruction, metrics/logging, and DLQ/backpressure visibility.
- Adding unbounded queues, vectors, maps, or blocking I/O to actor hot paths.
- Weakening atomic ordering because a local test appears to pass.
