# Scheduler Placement and Coroutine Runtime Decoupling Design

## Summary

`src/sched/scheduler.cpp` currently combines two different scheduling concerns:

1. **Worker placement scheduling**: route ready actors to shared worker queues,
   EDF queues, pinned test queues, work stealing, and dedicated pools.
2. **Actor activation scheduling**: decide how an actor runs once a worker has
   selected it, including behavior-message dispatch, actor state transitions,
   mailbox requeue decisions, and C++20 coroutine resume/suspend handling.

This design keeps the public `IScheduler` API and `HybridScheduler` facade
source-compatible, but splits the implementation into smaller scheduler
subsystems. The worker-pool scheduler will stop knowing whether an actor is a
behavior actor or coroutine actor. A separate actor execution layer will own
state transitions and activation mode selection, and a coroutine runtime will
own coroutine-specific resume/suspend contracts.

## Current Problems

### Mixed responsibilities in `HybridScheduler`

`HybridScheduler::notify_ready()` currently:

- Rejects work when the scheduler is stopped.
- Detects dedicated-thread and dedicated-pool actors.
- Performs the actor state gate for event-based actors.
- Chooses a worker by pinning or round-robin.
- Pushes to Chase-Lev priority queues, EDF queues, or pinned side queues.

`HybridScheduler::execute_actor()` then:

- Emits scheduler dispatch metrics and logs.
- Sets memory accounting thread-local actor state.
- Selects behavior versus coroutine execution using `system_.use_coroutines()`.
- Mutates `ActorState`.
- Starts and resumes actor coroutines.
- Pops mailbox messages for behavior actors.
- Applies dequeue-time deadline expiry.
- Directly requeues behavior actors by pushing to `workers_[...]`.

This makes the thread-pool scheduling algorithm depend on actor execution
semantics. The direct requeue path is the strongest coupling: it bypasses
`notify_ready()`, duplicating worker selection and skipping pinned/dedicated
routing decisions.

### Coroutine scheduling is not isolated

Coroutine handling leaks through several layers:

- `HybridScheduler::execute_actor()` includes coroutine headers and directly
  calls `ActorCoroutine::resume()`.
- `TimerAwaiter` depends on `HybridScheduler&`, not a smaller timer/readiness
  interface.
- `CoroutinePromise` stores execution details such as `WorkerThread* owner`,
  while the current scheduler does not consistently use that ownership.
- `MPSCActorMailbox` has a disabled continuation callback because direct
  continuation resume races with the scheduler state machine. The intended
  safe wakeup path is already readiness notification, not direct resume.

### Test and maintenance friction

- `process_actor()` duplicates mailbox pop/deadline behavior and appears
  superseded by `execute_actor()`.
- `worker_loop()` calls `pop_local()`, which already checks EDF, then calls
  `pop_edf()` again.
- Deterministic test controls (`pause_workers()`, `run_one_ready()`,
  `run_actor()`) call the same monolithic `execute_actor()` and inherit all
  mixed behavior.
- Focused tests for worker placement cannot avoid coroutine concerns, and
  coroutine tests cannot exercise the coroutine runtime without the full worker
  router.

## Goals

- Preserve `IScheduler` and `ActorSystem` source compatibility for existing
  users.
- Keep `HybridScheduler` as the public scheduler facade for now.
- Make worker placement independent of behavior dispatch and coroutine resume.
- Make behavior-message execution and coroutine execution share one actor
  activation contract.
- Ensure all requeue paths go through the same placement router.
- Keep no-exception and no-RTTI constraints.
- Preserve current actor state safety: at most one activation of an
  event-based actor runs at a time.
- Preserve deterministic test controls for paused workers and pinned actors.
- Add scheduler/concurrency tests before changing behavior.

## Non-Goals

- Do not replace the work-stealing algorithm.
- Do not change public actor APIs.
- Do not change mailbox admission or overflow semantics.
- Do not make coroutine mode the default.
- Do not introduce a new event loop or OS-thread abstraction.
- Do not remove dedicated-thread or dedicated-pool support.

## Recommended Architecture

Use a facade split. `HybridScheduler` remains the object behind `IScheduler`,
but it delegates to three internal subsystems:

```text
Mailbox / ActorSystem / Timer callback
        |
        v
IScheduler::notify_ready(actor, priority, deadline)
        |
        v
ActorReadyGate
    - validates event-based actor state
    - performs Idle/IOWaiting -> Ready admission
    - rejects Ready/Running/Terminated duplicates
        |
        v
WorkPlacementScheduler
    - dedicated-thread suppression
    - dedicated-pool routing
    - pinned worker routing
    - round-robin worker assignment
    - EDF and priority queue insertion
    - A2WS stealing and worker backoff
        |
        v
worker_loop pops WorkItem
        |
        v
ActorExecutionEngine
    - logs/metrics dispatch
    - sets memory accounting current actor
    - selects execution mode
        |
        +--> BehaviorActorRunner
        |       - pop one mailbox message
        |       - deadline expiry
        |       - actor->receive()
        |       - lost-wakeup requeue decision
        |
        +--> CoroutineActorRunner
                - ensure coroutine started
                - Ready -> Running
                - resume coroutine
                - observe suspend/terminate state
```

The important boundary is that `WorkPlacementScheduler` only moves
`WorkItem`s between queues and workers. It never calls `receive()`, never
resumes a coroutine, and never interprets actor mode.

## Components

### `ActorReadyGate`

`ActorReadyGate` owns readiness admission for event-based actors.

Responsibilities:

- Look up the actor from `ActorSystem`.
- Ignore non-existent actors.
- For non-event-based actors, allow the ready item through unchanged. This
  preserves existing permissive behavior for scheduler API calls.
- For event-based actors, CAS the state from `Idle` or `IOWaiting` to `Ready`.
- Reject actors already in `Ready`, `Running`, or `Terminated`.

Proposed interface:

```cpp
enum class ReadyAdmissionCode : uint8_t {
    Accepted,
    MissingActor,
    AlreadyReady,
    AlreadyRunning,
    Terminated,
};

struct ReadyAdmission {
    ReadyAdmissionCode code;
    bool accepted() const noexcept { return code == ReadyAdmissionCode::Accepted; }
};

class ActorReadyGate {
  public:
    explicit ActorReadyGate(ActorSystem& system) noexcept;

    ReadyAdmission try_mark_ready(ActorId actor) noexcept;
    void mark_ready_already_admitted(EventBasedActor& actor) noexcept;
};
```

`mark_ready_already_admitted()` is only for actor execution code that has
already proven it owns the actor activation, such as behavior requeue after
processing one message or coroutine yield. It prevents public `notify_ready()`
from needing to accept `Ready` actors, while still keeping requeue paths out of
worker queue internals.

### `WorkPlacementScheduler`

`WorkPlacementScheduler` owns the M:N worker algorithm.

Responsibilities:

- Maintain worker queue state: Chase-Lev priority queues and EDF queues.
- Maintain A2WS victim selection and steal metrics.
- Maintain pinned actor routing for deterministic tests.
- Maintain dedicated pool routing and dedicated-thread suppression.
- Provide `enqueue_admitted(WorkItem item)` for work that has already passed
  `ActorReadyGate`.
- Provide paused-worker drain helpers without calling actor code directly.

This can start as a private helper inside `scheduler.cpp` and move into
`src/sched/work_placement_scheduler.cpp` once the split is stable. The public
`HybridScheduler` should become a thin facade over this helper.

Proposed interface:

```cpp
struct PlacementContext {
    uint32_t current_worker_id;
    bool workers_paused;
};

class WorkPlacementScheduler {
  public:
    void start();
    void stop();

    void enqueue_admitted(const WorkItem& item);
    bool pop_local(uint32_t worker_id, WorkItem& out);
    bool try_steal(uint32_t thief_worker_id, WorkItem& out);
    bool pop_any_for_test(WorkItem& out);
    bool pop_one_on_worker_for_test(uint32_t worker_id, WorkItem& out);

    void pin_actor_to_worker(ActorId actor, uint32_t worker_id);
    void unpin_actor(ActorId actor);
    bool take_pinned_for_test(ActorId actor, WorkItem& out, uint32_t& worker_id);
};
```

The placement layer must not include `event_based_actor.hpp`,
`coroutine_task.hpp`, or `coroutine_awaiters.hpp`.

### `ActorExecutionEngine`

`ActorExecutionEngine` owns activation after a worker has selected a
`WorkItem`.

Responsibilities:

- Look up the actor and mailbox.
- Reject non-event-based actors.
- Emit `kSchedulerDispatch` metrics and scheduler dispatch logs.
- Set and restore `mem::current_actor_id()` around actor code.
- Select `BehaviorActorRunner` or `CoroutineActorRunner`.
- Return requeue intent instead of directly pushing into worker queues.

Proposed interface:

```cpp
struct ActorExecutionContext {
    uint32_t worker_id;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics;
    log::Logger* logger;
};

enum class ActorRunDisposition : uint8_t {
    Skipped,
    SuspendedOrIdle,
    RequeueReady,
    Terminated,
};

struct ActorRunResult {
    ActorRunDisposition disposition;
    uint8_t priority;
    int64_t deadline_ns;
};

class ActorExecutionEngine {
  public:
    ActorExecutionEngine(ActorSystem& system, ActorReadyGate& ready_gate) noexcept;

    ActorRunResult run(const WorkItem& item,
                       const ActorExecutionContext& context) noexcept;
};
```

`HybridScheduler::worker_loop()` becomes:

```cpp
if (placement_.pop_local(worker_id, item) ||
    placement_.try_steal(worker_id, item)) {
    mark_dispatch_begin();
    auto result = executor_.run(item, context_for(worker_id));
    mark_dispatch_end();
    if (result.disposition == ActorRunDisposition::RequeueReady) {
        placement_.enqueue_admitted(
            WorkItem{item.actor, result.deadline_ns, next_sequence()});
    }
}
```

### `BehaviorActorRunner`

`BehaviorActorRunner` owns behavior-message activation.

Responsibilities:

- Transition `Ready -> Running`.
- Pop at most one mailbox message per activation, preserving current fairness.
- Drop expired messages before `actor->receive()`.
- Call `actor->receive(msg)` for live messages.
- If the mailbox still has work, set `Running -> Ready` and return
  `RequeueReady`.
- If the mailbox appears empty, set `Running -> Idle`, then perform the existing
  lost-wakeup double check. If a message arrived during the race, CAS
  `Idle -> Ready` and return `RequeueReady`.
- Never push directly to worker queues.

This runner should absorb the currently duplicated deadline-expiry code from
`process_actor()` and behavior-mode `execute_actor()`.

### `CoroutineActorRunner`

`CoroutineActorRunner` owns coroutine activation.

Responsibilities:

- Compile only when `HPACTOR_SUPPORT_COROUTINES` is true.
- Lazily call `EventBasedActor::ensure_coroutine_started()`.
- Transition `Idle` or `IOWaiting` to `Ready` only through the readiness gate.
- Transition `Ready -> Running` before resume.
- Resume the actor coroutine.
- Treat `Idle` and `IOWaiting` after resume as suspended states.
- Call `actor->on_exit()` on termination exactly once.
- Never choose a worker and never push to worker queues.

Coroutine wakeups remain readiness-based:

- `MailboxAwaiter` suspends by storing continuation and setting actor state to
  `Idle`.
- `TimerAwaiter` suspends by setting actor state to `IOWaiting`.
- Mailbox enqueue or timer expiry calls readiness notification.
- A worker later resumes the coroutine through `CoroutineActorRunner`.

Direct continuation resume from `MPSCActorMailbox` should stay disabled unless a
future design introduces a separate single-threaded coroutine event loop. The
current thread-pool execution model requires readiness notification to preserve
the actor state machine.

### Awaiter Interface Cleanup

`TimerAwaiter` should not depend on `HybridScheduler&`. Replace that coupling
with smaller interfaces:

```cpp
class IActorReadyNotifier {
  public:
    virtual ~IActorReadyNotifier() = default;
    virtual void notify_ready(ActorId actor, uint8_t priority,
                              int64_t deadline_ns) = 0;
};

class ITimerService {
  public:
    virtual ~ITimerService() = default;
    virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;
    virtual void cancel_timer(TimerHandle handle) = 0;
};
```

`IScheduler` can implement both interfaces, but awaiters should only include
the smaller headers. This removes coroutine headers from the worker placement
layer and makes coroutine tests possible without a full `HybridScheduler`.

### Yield Semantics

The current `IScheduler::yield(actor, priority)` delegates to `notify_ready()`,
which can reject a currently `Running` actor. The refactor should make yield an
explicit actor-runtime handoff:

1. The coroutine yield awaiter verifies it is running on the actor activation.
2. It transitions `Running -> Ready`.
3. It asks the scheduler facade to route an already-admitted `WorkItem`.

Because the public `IScheduler` API should stay stable, implement this as an
internal path first:

```cpp
void HybridScheduler::enqueue_admitted_for_runtime(WorkItem item);
```

The method is private or package-internal; public `yield()` remains but uses the
same runtime path once the current actor state is known.

## Data Flow

### Message arrival

```text
ActorSystem::deliver_local()
    -> mailbox.try_push(msg, meta)
    -> empty-to-nonempty edge
    -> IScheduler::notify_ready(actor, meta.priority, meta.deadline_ns)
    -> ActorReadyGate::try_mark_ready(actor)
    -> WorkPlacementScheduler::enqueue_admitted(work)
    -> worker pops work
    -> ActorExecutionEngine::run(work, context)
```

### Behavior actor activation

```text
ActorExecutionEngine
    -> BehaviorActorRunner
    -> Ready -> Running
    -> mailbox.try_pop()
    -> expiry check
    -> actor.receive(msg)
    -> Running -> Ready and requeue, or Running -> Idle
```

### Coroutine actor activation

```text
ActorExecutionEngine
    -> CoroutineActorRunner
    -> ensure_coroutine_started()
    -> Ready -> Running
    -> coroutine.resume()
    -> coroutine sets Idle, IOWaiting, or Terminated
    -> no worker placement decision inside coroutine runner
```

## Concurrency Contract

- `ActorReadyGate::try_mark_ready()` is safe from any thread.
- `WorkPlacementScheduler::enqueue_admitted()` is safe from any thread after
  readiness admission.
- `WorkPlacementScheduler::pop_local()` is called only by the owning worker.
- `WorkPlacementScheduler::try_steal()` is safe from worker threads and test
  drain helpers.
- `ActorExecutionEngine::run()` is called by at most one worker per admitted
  `WorkItem`.
- `BehaviorActorRunner` and `CoroutineActorRunner` must acquire actor ownership
  with `Ready -> Running` CAS before executing actor code.
- Requeue from actor execution must return `RequeueReady` and go through
  `WorkPlacementScheduler::enqueue_admitted()`.
- Awaiters must not resume continuations from producer threads. They may store
  continuations and signal readiness.
- Timer callbacks must not block. They may signal readiness and return.

## Error Handling and Lifecycle

- Missing actors return `ActorRunDisposition::Skipped`.
- Missing mailboxes transition event-based actors back to `Idle` and return
  `Skipped`.
- Terminated actors are never admitted by `ActorReadyGate`.
- If an actor reaches `Terminated` during activation, the runner calls
  `on_exit()` once and returns `Terminated`.
- Expired messages continue to emit `kDeliveryExpired`.
- Existing lifecycle gates in `EventBasedActor::receive()` remain unchanged.

## Observability

Scheduler metrics and logs stay attached to the facade:

- `kSchedulerDispatch` is emitted immediately before actor execution.
- `kSchedulerSteal` remains in `WorkPlacementScheduler`.
- `kDeliveryExpired` moves into `BehaviorActorRunner` with the same event fields.
- Logs include the worker id from `ActorExecutionContext`.

Future metrics should distinguish placement from execution:

- Placement: enqueue, steal, queue depth, pinned route, dedicated-pool route.
- Execution: activation started, activation skipped, activation requeued,
  coroutine suspended, actor terminated.

This design does not require adding those new metrics in the first refactor.

## Migration Plan

1. Add focused regression tests around current scheduler behavior:
   - paused workers process behavior actors through `run_one_ready()`;
   - pinned actors requeue through the pinned worker path;
   - `yield()` on a running actor is not lost;
   - coroutine mailbox and timer wakeups use readiness notification, not direct
     continuation resume.
2. Extract `ActorReadyGate` while leaving `HybridScheduler::notify_ready()`
   behavior unchanged.
3. Extract `WorkPlacementScheduler` from worker queue, EDF, A2WS, pinning, and
   dedicated-pool code.
4. Extract `ActorExecutionEngine` with behavior execution first.
5. Move coroutine execution into `CoroutineActorRunner`.
6. Replace direct worker queue pushes from behavior requeue with
   `enqueue_admitted()`.
7. Narrow awaiter dependencies from `HybridScheduler&` to timer/readiness
   interfaces.
8. Remove dead or duplicate code:
   - delete `process_actor()` if no caller remains;
   - remove the second `pop_edf()` call in `worker_loop()`;
   - remove unused coroutine owner plumbing if it remains unused.

Each step should preserve passing targeted scheduler tests before the next
step begins.

## Files Expected to Change During Implementation

Likely new files:

- `include/hpactor/sched/actor_ready_gate.hpp`
- `src/sched/actor_ready_gate.cpp`
- `include/hpactor/sched/actor_execution_engine.hpp`
- `src/sched/actor_execution_engine.cpp`
- `include/hpactor/sched/work_placement_scheduler.hpp`
- `src/sched/work_placement_scheduler.cpp`
- `include/hpactor/sched/scheduler_interfaces.hpp`

Likely modified files:

- `include/hpactor/sched/scheduler.hpp`
- `src/sched/scheduler.cpp`
- `include/hpactor/sched/coroutine_awaiters.hpp`
- `include/hpactor/sched/coroutine_task.hpp`
- `include/hpactor/actor/event_based_actor.hpp`
- `tests/unit/sched/*`
- `tests/integration/sched/*`
- `tests/support/scheduler_test_driver.hpp`

## Acceptance Criteria

- `HybridScheduler` no longer includes coroutine awaiter/task headers in the
  worker placement implementation.
- Worker placement code has no calls to `EventBasedActor::receive()`,
  `EventBasedActor::ensure_coroutine_started()`, or `ActorCoroutine::resume()`.
- Actor execution code has no direct access to `workers_`, pinned queues, or
  A2WS.
- Behavior requeue uses the same placement path as initial readiness.
- Coroutine wakeups are readiness based and do not resume from producer,
  mailbox, or timer callback threads.
- Existing public scheduler APIs still compile.
- Existing deterministic scheduler controls still work.
- Targeted scheduler, mailbox wakeup, and coroutine scheduling tests pass.

## Alternatives Considered

### A. Helper-only split inside `scheduler.cpp`

This would move behavior and coroutine branches into private helper methods but
keep all data and routing inside `HybridScheduler`.

Pros:

- Lowest short-term diff.
- Minimal CMake churn.

Cons:

- Does not decouple placement from activation.
- Direct requeue can still bypass placement policy.
- Tests still require the monolithic scheduler.

### B. Replace `HybridScheduler` with separate public schedulers

This would introduce a public `ThreadPoolScheduler` plus a public
`CoroutineScheduler` and make `ActorSystem` compose them explicitly.

Pros:

- Clean conceptual model.
- Public names match responsibilities.

Cons:

- Larger API churn.
- More risk to existing examples, config, and tests.
- Harder to land incrementally.

### C. Facade-preserving internal split

Keep `IScheduler` and `HybridScheduler` public, but split internals into
ready-gate, placement, actor execution, behavior runner, and coroutine runner.

Pros:

- Source-compatible.
- Incremental and testable.
- Addresses the coupling that matters most.
- Leaves room to rename or expose components later.

Cons:

- `HybridScheduler` remains a broad facade.
- Some internal interfaces are initially package-private rather than pure
  public abstractions.

Recommendation: choose option C.

## Review Questions

- Should coroutine mode remain a global `Config::use_coroutines` switch, or
  should the execution engine eventually select coroutine mode per actor?
- Should `IScheduler::yield()` remain public, or should yield become an actor
  runtime API exposed through `ActorContext`?
- Should timer service stay inside `HybridScheduler` during this refactor, or
  should timer ownership become a separate `TimerService` in the same change?

