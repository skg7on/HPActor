<!--
Copyright 2026 HPActor Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Actor Concurrency and Lock-Free Mailbox Rules

Status: normative development rules.

Audience: HPActor feature designers, implementers, reviewers, and test authors.

This document captures the concurrency rules that must hold when adding actor,
mailbox, scheduler, delivery, lifecycle, observability, or production
reliability features. It is grounded in the current architecture docs and the
current mailbox and scheduler implementation:

- `docs/architecture/system-architecture-and-key-concept-high-level-design.md`
- `docs/architecture/core/actor-core-concept.md`
- `docs/architecture/actor/mailbox-management-backpressure-design.md`
- `docs/architecture/scheduling/scheduling-subsystem-design.md`
- `docs/architecture/scheduling/scheduling-architecture-design.md`
- `docs/architecture/scheduling/scheduling-mathematical-model.md`
- `docs/architecture/production/priority-mailbox-lanes-design.md`
- `include/hpactor/adt/mpsc_mailbox.hpp`
- `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`
- `include/hpactor/mailbox/multi_lane_queue.hpp`
- `include/hpactor/sched/actor_ready_gate.hpp`
- `include/hpactor/sched/actor_execution_engine.hpp`
- `include/hpactor/sched/work_placement_scheduler.hpp`
- `src/sched/actor_ready_gate.cpp`
- `src/sched/actor_execution_engine.cpp`
- `src/sched/scheduler.cpp`
- `src/sched/work_placement_scheduler.cpp`
- `src/sched/worker_thread.cpp`

## Core Model

HPActor uses turn-based actor concurrency. An event-based actor owns its state
while it is running one activation, and other actors communicate with it only by
message delivery. Feature designs must preserve these properties:

1. Actor state is private to the actor. Do not expose mutable actor state to
   another actor, scheduler worker, network callback, timer callback, metrics
   drain, or test helper.
2. Event-based actors process messages through the actor runtime. Do not call
   `receive()` directly from a feature path except inside the scheduler-owned
   execution engine or a narrowly scoped test double.
3. Actor handlers must not block scheduler workers. Long blocking work belongs
   in `BlockingActor`, `DaemonActor`, a dedicated pool, an event-loop callback,
   or another explicit asynchronous boundary.
4. Dedicated actors still have actor boundaries. A dedicated thread or pool
   changes where actor execution runs; it does not permit shared mutable state
   or direct mailbox consumption by arbitrary threads.
5. Actor lifecycle, drain, supervision, quarantine, and delivery features must
   preserve source-compatible defaults. Production behavior should be opt-in or
   safely defaulted.

## Delivery Boundary Rules

The production-facing local delivery boundary is
`ActorSystem::try_deliver_local()`. That path performs actor lookup, quarantine
and circuit-breaker admission, delivery deadline handling, receiver-side
deduplication, mailbox metadata construction, bounded admission, failure
observability, DLQ handoff, and backpressure signaling.

Use these rules for new features:

1. User and production-facing sends should enter through `ActorContext::send`,
   `ActorContext::try_send`, `ActorContext::send_with_priority`,
   `ActorContext::try_send_with_priority`, `ActorSystem::deliver_local`, or
   `ActorSystem::try_deliver_local`.
2. Do not bypass `try_deliver_local()` for production features unless the design
   explicitly states which delivery semantics are intentionally skipped.
3. Direct mailbox calls such as `MPSCActorMailbox::enqueue()` and
   `MPSCActorMailbox::inject_for_test()` are for subsystem internals, legacy
   compatibility, or tests that deliberately bypass delivery semantics.
4. `LocalDeliveryEngine` is a narrow helper that directly enqueues a released
   `TypedMessage*`. Do not use it as the normal production delivery path for
   features that need TTL, dedup, DLQ, tracing, backpressure, quarantine, or
   failure envelopes.
5. Preserve sender, trace context, priority, deadline, message id, delivery
   flags, and failure metadata when adding delivery features.

## MPSC Queue Rules

`hpactor::adt::MPSCMailbox<T>` is an intrusive Vyukov-style MPSC queue. It is
safe for many concurrent producers and exactly one serialized consumer.

Design and implementation rules:

1. `T` must contain `std::atomic<T*> mpsc_next`. The queue owns only the link
   protocol, not the message allocation policy.
2. Producers may call `enqueue()` concurrently. Producer-side enqueue is the
   only multi-producer operation.
3. Only one consumer context may call `dequeue()` or `try_dequeue()` at a time.
   If more than one code path can consume or evict, the owner must serialize
   those paths externally.
4. A node address must not be re-enqueued while it is still reachable from any
   queue lane. Reuse only after the consumer has dequeued the node and completed
   ownership transfer.
5. The consumer owns node destruction after dequeue. For `TypedMessage`
   mailboxes, current code uses `mem::allocate()` for nodes and destroys them
   with explicit destructor plus `mem::deallocate()`.
6. Do not add a second consumer to an actor mailbox to make a feature faster.
   That violates the queue contract and the actor model.
7. `empty()` and `count()` are snapshots. They are useful for admission,
   pressure, observability, and lost-wakeup checks, but they are not stable
   iteration guards under concurrent producers.
8. The queue's acquire/release ordering is part of the contract. Do not weaken
   atomic ordering or replace it with relaxed operations without a written
   proof and stress/model-checker tests.

## Actor Mailbox Orchestrator Rules

`MPSCActorMailbox<T>` is the actor mailbox orchestrator. It combines bounded
admission, reservation accounting, pressure state, overflow policy, multi-lane
storage, metrics, logging, edge-triggered scheduler wakeup, and consumer
serialization.

Rules:

1. Add mailbox behavior in `MPSCActorMailbox` or a subsystem-owned helper, not
   by reaching into `MPSCMailbox` lanes from unrelated code.
2. Admission must reserve bounded capacity before user-message enqueue. Keep
   `ReservationManager` count and byte accounting paired with every successful
   user-message dequeue, drop, spill, or failed follow-up reservation.
3. System-lane messages bypass user-message reservations, but they are still
   bounded by `protected_system_messages` and system-lane byte accounting.
4. Overflow handlers must not consume queue lanes directly. Use mailbox-owned
   callbacks such as `drop_one_oldest_global()` or `drop_one_lowest_priority()`
   so the consumer lock, reservations, metrics, and deferred destruction stay
   consistent.
5. The mailbox's `consumer_lock_` serializes normal dequeue, overflow eviction,
   and overflow drain. Preserve that single-consumer serialization when adding
   new drain, replay, inspection, or eviction behavior.
6. Do not hold the consumer lock across actor `receive()`, scheduler calls,
   user callbacks, blocking I/O, logging sinks that may block, or remote
   transport calls.
7. `enqueue_reserved()` performs the edge-triggered wakeup. If changing wakeup
   behavior, preserve the quiet-period invariant: the first enqueue after
   empty claims `mailbox_was_empty_` and notifies the scheduler; later enqueues
   in the same non-empty period do not create duplicate runnable entries.
8. Fault injection hooks in mailbox paths must not leak reserved capacity or
   allocated nodes. Any new hook must state whether it fires before reservation,
   after reservation, before allocation, or after allocation.
9. Mailbox snapshots are observability snapshots. Tests may assert stable
   values only when producers and consumers are quiesced or controlled.

## Multi-Lane Queue Rules

`MultiLaneQueue<T>` owns one protected system lane and up to eight user lanes,
each backed by an MPSC queue.

Rules:

1. Producers may enqueue into a selected lane concurrently.
2. `MultiLaneQueue` does not own a consumer lock. Its `dequeue()`,
   `try_drop_oldest_user_lane()`, `try_drop_from_lowest_user_lane()`, `reset()`,
   and pending-free methods require external consumer serialization.
3. System lane uses `MultiLaneQueue<T>::kSystemLaneSentinel`. User lanes are
   `0..num_user_lanes-1`, where `0` is the highest priority.
4. Dequeue order is always system lane first, then user lanes from highest to
   lowest priority.
5. FIFO ordering is preserved only within a lane. Cross-lane ordering is
   priority ordering, not global enqueue order.
6. When `priority_aware` is false, all user messages route to user lane 0.
   When it is true, user priority is capped to the configured lane count.
7. System messages are detected by TypeTags below `TypeTag::User` and route to
   the protected system lane regardless of `priority_aware`.
8. `DropLowestPriority` must not evict system messages. It should evict from the
   lowest-priority non-empty user lane and rely on mailbox-owned accounting.
9. Resizing user lanes must not silently orphan queued messages. If a future
   feature allows shrink while live messages exist in removed lanes, the design
   must define drain, move, or reject semantics first.

## Scheduler and Ready-State Rules

The actor scheduling contract is represented by `ActorState`,
`ActorReadyGate`, `ActorExecutionEngine`, `HybridScheduler`, and
`WorkPlacementScheduler`.

Current event-based actor states are:

- `Idle`: no admitted activation is running or queued.
- `Ready`: actor has an admitted scheduler work item.
- `Running`: a worker or dedicated execution context owns the actor activation.
- `IOWaiting`: actor is suspended on an I/O or timer boundary.
- `Terminated`: actor must not be scheduled.

Rules:

1. Public readiness notifications must go through
   `ActorReadyGate::try_mark_ready()`. It accepts only `Idle` and `IOWaiting`
   event-based actors.
2. Owned requeues must go through
   `ActorReadyGate::mark_ready_already_admitted()`. It is for code that already
   owns the activation, such as requeue after an execution slice or coroutine
   yield.
3. Do not set `ActorState` directly in production feature code unless the code
   is part of actor lifecycle, ready-gate, execution-engine, or a documented
   actor-mode transition. Tests may set state directly only to isolate the
   transition under test.
4. A work item does not by itself grant execution rights. The execution runner
   must still win the `Ready -> Running` CAS before calling actor code.
5. A failed `Ready -> Running` CAS means another state transition won. Do not
   recover by calling `receive()` anyway.
6. After a behavior actor processes one message, the runner checks whether the
   mailbox still has work. If it does, it requeues through the owned-admission
   path. If it looks empty, it sets `Idle` and performs a lost-wakeup
   double-check before returning idle.
7. `WorkPlacementScheduler` moves only `WorkItem` values. It must not inspect
   or mutate actor internals.
8. Scheduler priority and mailbox priority are related but distinct. Mailbox
   lanes choose dequeue order inside an actor; scheduler priority chooses worker
   placement and ready-queue ordering across actors.
9. Dedicated-thread actors are registered with the scheduler and suppressed
   from shared placement. Feature code must not enqueue their activations onto
   cooperative queues unless the design changes that contract explicitly.

## Multi-Threaded Actor Programming Rules

For actor authors and runtime feature code:

1. Treat actor handlers as single-threaded turns over private state.
2. Send immutable or ownership-transferred data. Do not send pointers or
   references to mutable state unless the target actor becomes the only owner.
3. Never wait synchronously for another event-based actor from inside an
   event-based actor handler. Use request/reply, continuation state, scheduled
   self-delivery, or a blocking actor.
4. Timer, network, and external callbacks should deliver messages into actors.
   They should not mutate actor state directly.
5. Metrics and logs must be out-of-band and bounded. Instrumentation must not
   block scheduler workers or allocate unbounded memory on actor hot paths.
6. Cross-thread caches and snapshots must be documented as approximate unless
   they are protected by a lock or use a stronger synchronization contract.
7. Use existing memory regions for actor, message, coroutine, network,
   hibernate, and internal allocations. Preserve actor attribution where
   `mem::set_current_actor_id()` or mailbox allocation already provides it.

## Feature Design Checklist

Every feature that touches actor delivery, mailbox state, scheduler state, or
threaded execution must answer these questions in its design:

1. What is the actor-state transition contract?
2. Which thread or component is the single consumer for each queue touched?
3. Where are message nodes allocated, and who destroys them?
4. Where is bounded capacity reserved and released?
5. Does the feature preserve delivery metadata: sender, trace, priority,
   deadline, message id, and flags?
6. What happens on overflow, rejection, expiration, actor-not-found, mailbox
   closed, and circuit-open paths?
7. How are DLQ, metrics, logging, tracing, CLI/admin inspection, and
   backpressure updated?
8. Does any new callback run on a scheduler worker, timer thread, event-loop
   thread, dedicated actor thread, or producer thread?
9. What operations are permitted while the actor is draining, stopping,
   terminated, quarantined, or I/O waiting?
10. Which tests prove no duplicate execution, no lost wakeup, bounded memory,
    and correct ordering under concurrency?

## Implementation Rules

1. Prefer subsystem-owned extension points over central switches. For mailbox
   policy, extend overflow handlers, admission helpers, or lane helpers instead
   of adding unrelated branches to `ActorSystem`.
2. Keep hot paths branch-light and allocation-aware. Admission should reject or
   route before avoidable allocation when practical.
3. Preserve no-exceptions and no-RTTI constraints. Do not introduce
   `dynamic_cast`, `typeid`, or exception-based control flow.
4. Do not add blocking I/O to event loops, cooperative workers, mailbox
   admission, ready-gate, or scheduler placement.
5. Use explicit failure results. A dropped, rejected, expired, or rerouted
   message must be observable through the relevant result, metric, log, DLQ, or
   backpressure path.
6. Keep new lock-free code small and locally documented. State producer roles,
   consumer roles, linearization points, memory ordering, node lifetime, and
   progress guarantees.
7. Prefer existing primitives: `MPSCMailbox`, `MultiLaneQueue`,
   `ReservationManager`, `OverflowQueue`, `ActorReadyGate`,
   `WorkPlacementScheduler`, `TimingWheel`, `CalendarQueue`, and existing
   metrics/log/tracing ring buffers.
8. Avoid mixed ownership. A component that reserves capacity should release it,
   or it should call a mailbox-owned helper that releases it.

## Test Design Rules

Use the narrowest deterministic test that proves the contract.

### Unit Tests

Use unit tests for:

1. MPSC or lane ordering when producer and consumer execution is controlled.
2. Admission results: accepted, soft pressure, rejected, dropped, overflow,
   DLQ, and system-lane full.
3. Reservation accounting and byte accounting.
4. Ready-gate state transitions.
5. Placement rules: EDF before priority, pinned actors, dedicated suppression,
   and work stealing helpers.
6. Snapshot fields after producers and consumers are quiesced.

Rules:

1. Use mock schedulers for mailbox-only tests when you only need to observe
   `notify_ready()`.
2. Use `scheduler_threads = 0` when a test needs to inspect mailbox or actor
   state before workers can drain it.
3. Use `scheduler_start_paused = true`, `run_one_ready()`, `drain_ready()`,
   `pin_actor_to_worker()`, and `run_actor()` for deterministic scheduler tests.
4. Use `inject_for_test()` only when the test intentionally bypasses edge
   triggering or admission. Say so in the test name or comment.
5. Assert stable ordering only within a lane unless the test explicitly covers
   system-first or priority-lane ordering.

### Integration Tests

Use integration tests for:

1. `ActorContext` and `ActorSystem::try_deliver_local()` behavior across actor,
   mailbox, scheduler, metrics, tracing, DLQ, or backpressure boundaries.
2. Scheduler execution of real actors with paused workers.
3. Overflow policy interactions with actor execution.
4. Priority and deadline behavior that involves both mailbox metadata and
   scheduler placement.

Rules:

1. Avoid sleeps as proof. Prefer paused workers, condition-based polling with
   generous timeouts, or direct scheduler control.
2. If a real worker thread must run, poll for an observable condition with a
   timeout of at least five seconds unless the test is purely local and
   deterministic.
3. Do not assert on scheduler interleaving unless the scheduler is paused and
   you are driving it explicitly.
4. Keep test actors small. Use atomic counters only for observations crossing
   test threads, not as a substitute for actor state ownership.

### Stress, Race, and Model Tests

Use stress or race-oriented tests when changing:

1. `MPSCMailbox`, `MultiLaneQueue`, `MPSCActorMailbox`, reservation accounting,
   overflow eviction, or DLQ handoff.
2. `ActorReadyGate`, actor state transitions, coroutine awaiters, yield, or
   lost-wakeup handling.
3. `ChaselevDeque`, `MultiPriorityWorkQueue`, EDF queues, worker placement,
   work stealing, or dedicated execution.
4. Timer, event-loop, transport, or callback delivery paths.

Rules:

1. For MPSC queue algorithm changes, add or update Relacy tests under
   `tests/unit/mailbox/test_mpsc_relacy.cpp`.
2. For mailbox overload, account for every attempted message as accepted,
   rejected, dropped, rerouted, or drained.
3. For bounded capacity, assert depth and max depth never exceed configured
   limits after concurrent producer runs.
4. For actor scheduling, assert no duplicate execution and no lost wakeup by
   controlling worker admission and requeue paths.
5. Run sanitizer or race-oriented tests when practical for scheduler, lock-free,
   mailbox, timer, or transport changes.

## Anti-Patterns

Avoid these patterns in designs, implementation, and tests:

1. Adding a second consumer for an MPSC actor mailbox or lane.
2. Calling `receive()` directly from a sender, timer, event loop, test producer,
   or backpressure path.
3. Mutating actor state from callbacks instead of sending a message.
4. Using sleeps to prove scheduler progress when paused-worker controls can
   drive the condition directly.
5. Treating `empty()`, `count()`, or snapshots as linearizable under concurrent
   mutation.
6. Reusing or freeing a message node while it may still be linked from a queue.
7. Dropping messages without updating reservation accounting and observability.
8. Bypassing `try_deliver_local()` for production-facing delivery features.
9. Adding unbounded queues, vectors, maps, or log buffers to actor hot paths.
10. Weakening atomic ordering without a written concurrency proof and focused
    verification.

## Review Gate

Before merging mailbox, scheduler, or actor-concurrency changes, reviewers
should require:

1. A written concurrency contract in the design or code comments for any new
   queue, cross-thread state machine, callback, or ownership transfer.
2. Evidence that every MPSC queue still has one serialized consumer.
3. Evidence that actor readiness cannot create duplicate runnable entries.
4. Evidence that no message admission path leaks capacity, memory, trace
   metadata, failure metadata, or backpressure visibility.
5. Deterministic tests for local behavior and stress/race tests for concurrent
   behavior proportional to the change risk.
