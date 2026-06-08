---
name: hpactor-deterministic-tests
description: Use when the user is writing, fixing, or asking how to test any HPActor component: actors, mailboxes (MPSC, bounded), timers (schedule, CalendarQueue), schedulers, coroutine awaiters (MailboxAwaiter), or fault injection. This covers requests for deterministic tests — those that avoid real threads, sleep-based waits, wall-clock delays, OS-specific scheduling, and CI-only failures.
---

# HPActor Deterministic Tests

Design HPActor tests so the result does not depend on wall-clock timing, OS
thread scheduling, CPU speed, macOS/Linux differences, or repeated reruns.

## Required Reads (Conditional)

Always read `AGENTS.md` and `CLAUDE.md` for repo conventions.

Read these only when they apply to the subsystem under test:

- **Actor, scheduler, mailbox, or timer tests:**
  `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- **Coroutine tests:**
  `tests/unit/sched/test_mailbox_awaiter.cpp` (mock scheduler pattern),
  `include/hpactor/coroutine/coroutine_awaiters.hpp`
- **Fault injection tests:**
  `tests/unit/fault/test_fault_schedule.cpp`,
  `tests/integration/fault/test_fault_seed_replay.cpp`
- **Test infrastructure:**
  `tests/support/scheduler_test_driver.hpp` (SchedulerTestDriver API),
  `tests/support/system_test_fixture.hpp` (SystemTestFixture)

Work in the required `.worktrees/` checkout before writing or changing tests.

## Lessons From Past Flakes

These PRs demonstrate the cost of non-deterministic tests:

- **PR #112 (timer tests)**: CI-only failures because delivery depended on
  timer-thread startup and wall-clock delay. Fix separated API behavior from real
  timer delivery, used `scheduler_threads = 0`, and inspected mailboxes directly.
- **PR #117 (concurrent stress)**: gcc/Linux-only failure from thread interleaving
  dependence and alignment UB. Fix replaced stress loops with deterministic
  single-threaded write/read-back coverage.
- **PR #142 (scheduler tests)**: timeout/crash from kernel worker scheduling
  dependence and fake actor IDs fed to real workers. Fix used
  `scheduler_start_paused`, `SchedulerTestDriver`, `pin_actor_to_worker()`,
  `run_actor()`, and `scheduler_threads = 0`.

Each failure shares a root cause: the test depended on an uncontrolled async
source. The fix always removes that source or replaces it with deterministic
control.

## Design Workflow

### 1. Classify The Async Surface

Before writing assertions, state which async source the test touches:

- **No async runtime**: pure unit behavior — construct objects directly.
- **Actor delivery or mailbox state**: messages flow through mailbox queues.
- **Cooperative scheduler worker execution**: workers drain ready queues.
- **Timer thread or scheduled delivery**: timer subsystem fires on a real OS thread.
- **Coroutine suspend/resume**: coroutine frames, promise state, awaiters.
- **Dedicated-thread actors** (`BlockingActor`, `DaemonActor`): real OS threads.
- **Network/event-loop callbacks**: kqueue/epoll edge-triggered I/O.
- **Fault injection**: fault points fire at specific ticks/domains.
- **Lock-free structure correctness**: concurrent producer/consumer races.

If the test does not intentionally verify the async source itself, remove that
source from the test.

### 2. Pick The Deterministic Control

Use the narrowest deterministic surface for the behavior under test:

- **Pure object behavior**: construct the object directly; no `ActorSystem`.
- **Mailbox/admission/state inspection**: `Config cfg; cfg.scheduler_threads = 0;`
  then inspect mailbox or actor state before any worker can drain it.
- **Mock scheduler**: implement a minimal `IScheduler` stub that records
  `notify_ready()` calls without real threads. Use this for mailbox-only,
  awaiter, or ready-gate tests that only need to observe scheduler
  notifications. See `tests/unit/sched/test_mailbox_awaiter.cpp` for the
  canonical pattern (`MailboxAwaiterMockScheduler`).
- **Actor processing with scheduler control**: set `scheduler_start_paused = true`,
  create `hpactor::test::SchedulerTestDriver`, then drive execution with
  `run_one()`, `drain()`, or `drain_until()`.
- **Specific actor or worker ordering**: use `pin_actor_to_worker()`,
  `run_actor()`, and `run_one_on_worker()`.
- **Timer arithmetic/data structure behavior**: prefer direct timer-backend unit
  tests with explicit advancement or controlled state.
- **Timer API behavior**: assert stable API outcomes such as valid handles, safe
  invalid cancel, idempotent cancel, and mailbox state after cancellation. Use
  `scheduler_threads = 0` so the timer thread never races observation.
- **Coroutine awaiter behavior**: use a mock scheduler, construct the awaiter
  directly with a manually-set `CoroutinePromise`, control mailbox contents with
  `inject_for_test()`, and call `await_ready()`/`await_suspend()`/`await_resume()`
  directly. No real coroutine actors, no `ActorSystem`.
- **Fault injection behavior**: use `FaultSchedule` with fixed tick positions
  and a known seed. Assert the exact sequence of fault firings, not that faults
  "eventually happen."
- **End-to-end timer delivery, network progress, or dedicated-thread behavior**:
  use condition-based polling on an observable condition with a generous
  timeout, and say why deterministic pumping cannot cover the behavior.

### 3. Assert Stable Facts

Assert facts that are stable under all supported platforms:

- Return values, failure codes, counters after controlled drains, mailbox
  contents, actor lifecycle states after the controlled transition, preserved
  metadata, bounded-depth accounting, and exact single-threaded ordering.
- Do not assert exact intermediate states if another thread can legally advance
  them before observation. Assert the invariant that matters instead.
- Do not assert global message ordering across priority lanes, workers, or
  stealing unless the test controls those lanes/workers explicitly.
- Do not use fake `ActorId` values with live scheduler workers. Use real
  registered actors or disable workers with `scheduler_threads = 0`.
- For coroutine tests: assert promise state flags, `await_ready()` return values,
  and mailbox contents — not that a coroutine "suspends within N ms."
- For fault tests: assert that a fault at tick N fires exactly once, not that it
  fires "eventually" or "around tick N."

### 4. Prove The Test Can Fail For The Right Reason

For new behavior, follow TDDFlow:

1. Write one focused test.
2. Run the narrowest command and observe the expected failure.
3. Implement the minimal change.
4. Re-run the same command and confirm it passes.
5. Re-run after refactor.

If rewriting a flaky test, first identify what uncontrolled source made it
flaky. The replacement test must remove that source or explicitly justify why
it remains.

## Red Flags

Treat these as design failures until justified:

- `sleep_for()` is used to make actor/scheduler/timer/coroutine progress happen.
- `assert_eventually()` is used where `SchedulerTestDriver` or
  `scheduler_threads = 0` would provide direct control.
- A test passes on macOS but fails on Ubuntu, gcc but not clang, debug but not
  release, or only after reruns.
- Assertion depends on a worker thread being scheduled within N milliseconds.
- Assertion checks an exact transient state owned by another thread.
- Test uses `std::thread` stress loops without a deterministic oracle.
- Test relies on `assert()`, which disappears under `NDEBUG`; use GTest
  `ASSERT_*`, `EXPECT_*`, or project `CHECK()` where appropriate.
- Test increases timeouts to "fix" flakes without removing the race.
- Test adds a CMake `TIMEOUT` for a short unit test instead of finding the hang
  or uncontrolled wait.
- Test spawns real coroutine actors and uses `sleep_for` to wait for
  suspend/resume — use a mock scheduler and direct awaiter construction instead.
- Test uses `spawn()` + `send()` + `sleep_for()` to test mailbox behavior that
  could be tested with `inject_for_test()` and `scheduler_threads = 0`.
- Fault test uses random seeds without recording them or expects faults to fire
  in a non-deterministic order.

## Replacement Patterns

### Replace Polling For Actor Processing

Bad:

```cpp
actor_ref.send(msg);
EXPECT_TRUE(test::assert_eventually([&] {
    return actor->handled_count() == 1;
}));
```

Good:

```cpp
cfg.scheduler_start_paused = true;
ActorSystem system(cfg);
hpactor::test::SchedulerTestDriver driver(system);

actor_ref.send(msg);
EXPECT_TRUE(driver.run_actor(actor->id()));
EXPECT_EQ(actor->handled_count(), 1);
```

### Replace Worker Scheduling Dependencies

Bad: create live workers, call `notify_ready(fake_actor_id)`, and expect the
queue result before a worker races through actor lookup.

Good: use `scheduler_threads = 0` for placement/admission state tests, or use
real actors with `scheduler_start_paused = true` and drive execution manually.

### Replace Real-Time Timer Assertions

Bad:

```cpp
auto h = ctx->schedule(100ms, msg);
std::this_thread::sleep_for(150ms);
EXPECT_EQ(actor->received(), 1);
```

Good:

```cpp
auto h = ctx->schedule(5s, msg);
EXPECT_NE(h.value(), 0u);
ctx->cancel_schedule(h);

auto* mailbox = system.get_mailbox(actor->id());
ASSERT_NE(mailbox, nullptr);
TypedMessage out;
EXPECT_FALSE(mailbox->try_pop(out));
```

Keep one higher-level integration scenario for timer delivery only when the
runtime path, not a unit API, is the behavior under test. That integration test
may use condition-based polling with a 5s+ timeout — and that's the only place
real timer delivery should appear.

### Replace Interleaving Stress With Deterministic Invariants

Bad: many threads loop thousands of times and only "pass if no crash".

Good: build a deterministic oracle. Acquire all resources, write sentinels,
release them, reacquire them, and verify identities, bounds, and preserved
contents. Add sanitizer, Relacy, or dedicated race tests only when changing the
lock-free algorithm itself.

### Replace Real Coroutine Actors With Direct Awaiter Tests

Bad:

```cpp
auto actor = system.spawn_coroutine<TestActor>(...);
std::this_thread::sleep_for(50ms);  // wait for suspend
system.send(actor->address(), msg);
std::this_thread::sleep_for(100ms);  // wait for resume
EXPECT_EQ(actor->received(), 1);
```

Good:

```cpp
// Mock scheduler — no real threads
MailboxAwaiterMockScheduler scheduler;
auto mb = std::make_unique<MPSCActorMailbox<TypedMessage>>(actor_id, &scheduler);

// Set up promise state directly
CoroutinePromise promise;
promise.actor_id = actor_id;
promise.state.set(ActorState::kRunning);
promise.mailbox_was_empty.store(true, std::memory_order_release);

// Test await_ready with empty mailbox
MailboxAwaiter<TypedMessage> awaiter(promise, mb.get());
EXPECT_FALSE(awaiter.await_ready());

// Inject a message and test immediate resume path
auto* msg = new TypedMessage(TypeTag::User, StreamBuffer{0x42});
mb->inject_for_test(msg);
MailboxAwaiter<TypedMessage> awaiter2(promise, mb.get());
EXPECT_TRUE(awaiter2.await_ready());
delete mb->dequeue();
```

### Replace Non-Deterministic Fault Tests

Bad:

```cpp
FaultController fc;
fc.enable_random(/*seed=*/std::random_device{}());
fc.run_with_faults([&] { ... });
EXPECT_TRUE(system.is_healthy());  // what faults fired? unknown.
```

Good:

```cpp
FaultSchedule schedule;
schedule.add_entry({FaultDomain::kMailbox, /*at_tick=*/3,
                    "hpactor.mailbox.enqueue.fail",
                    FaultAction::kFail, std::nullopt, FailPayload{-1}});
// Fixed schedule, fixed seed, deterministic replay
fc.load_schedule(schedule);
fc.run_with_faults([&] { ... });
// Assert exact outcomes for the known fault at tick 3
EXPECT_EQ(mailbox->rejected_count(), 1);
```

## Coroutine Test Determinism

Coroutine tests have a specific set of async sources: the C++20 coroutine
protocol (`await_ready`/`await_suspend`/`await_resume`), mailbox state, and
scheduler ready-state transitions. None of these require real OS threads.

**Default approach**: mock scheduler + direct awaiter construction.

1. Stub `IScheduler` with no-op methods. The mock only needs to exist — the test
   verifies that the awaiter calls the right methods, not that the scheduler
   processes work.
2. Construct `CoroutinePromise` directly and set `actor_id`, `state`, and
   `mailbox_was_empty` before each test case.
3. Use `inject_for_test()` to place messages in the mailbox without triggering
   edge-triggered wakeup. State in a comment that this bypasses admission and
   edge triggering — the test is verifying awaiter behavior, not mailbox
   admission.
4. Call `await_ready()`, `await_suspend()`, and `await_resume()` as regular
   method calls. No coroutine frame needed unless testing handle resumption.
5. For handle resumption tests: create a `std::coroutine_handle` from the
   promise and call `resume()` manually after injecting a message.

When you MUST test real coroutine scheduling (integration level): use
`scheduler_start_paused = true`, spawn the coroutine actor, drive one step with
`run_one()` to reach the suspend point, then inject or send the wakeup message
and `drain_until()` the expected outcome. Still no `sleep_for`.

## Fault Injection Determinism

Fault injection tests must be seed-replayable and tick-deterministic:

1. Use `FaultSchedule` with explicit tick positions, not random distributions.
2. When randomness is needed, record the seed and assert it in the test so CI
   can reproduce any failure.
3. Assert exact fault firing counts per domain, not "at least one fault fired."
4. For integration-level fault tests: use `scheduler_start_paused = true` so
   the tick counter advances deterministically with each `run_one()` call.
5. Separate fault point registration tests (pure unit — no faults fire) from
   fault activation tests (controlled schedule — faults fire at known ticks).

## When Real Time Or Threads Are Allowed

Real time and real OS threads are allowed only when the behavior under test is
itself a timer thread, event loop, network callback, dedicated-thread actor, or
concurrency primitive progress guarantee.

When allowed:

- Poll an observable condition, not elapsed time.
- Use a timeout of at least five seconds unless the test is purely local and
  deterministic.
- Use atomics only for cross-thread observations, not as replacement actor
  state.
- Join all test-created threads.
- Account for every operation outcome: accepted, rejected, dropped, drained,
  cancelled, timed out, or failed.
- Avoid exact interleaving assertions; assert invariants that must hold for all
  legal schedules.

**The timer thread is inherently non-deterministic.** You cannot control when the
OS delivers a timer signal. For timer API tests, use `scheduler_threads = 0` and
inspect mailbox state. For timer delivery tests, use condition-based polling
with a 5s+ timeout — and only write one such integration test per timer feature.
All other timer behavior should be covered by unit tests that never start the
timer thread.

## Review Checklist

Before accepting a test, verify:

### Test Identity
- The test name states one behavior.
- The test uses the narrowest tier: unit before integration before system.

### Async Surface
- [ ] No assertion depends on wall-clock delay, worker scheduling, or platform
  thread ordering unless that behavior is explicitly under test.
- [ ] The async source under test is explicitly identified. All other async
  sources are removed or controlled.

### Deterministic Control (pick at least one)
- [ ] `scheduler_threads = 0` for mailbox/state inspection tests.
- [ ] `scheduler_start_paused = true` + `SchedulerTestDriver` for actor
  processing tests.
- [ ] Mock scheduler for mailbox-only, awaiter, or ready-gate tests.
- [ ] Direct object construction for pure behavior tests.
- [ ] `inject_for_test()` for tests that bypass admission/edge-trigger (stated
  in test name or comment).

### Assertions
- [ ] Assertions verify stable facts: return values, counters after controlled
  drains, mailbox contents, lifecycle states, preserved metadata.
- [ ] No cross-worker ordering assertions without explicit worker control.
- [ ] No fake `ActorId` values with live scheduler workers.

### Special Subsystems
- [ ] Timer tests separate API semantics from real delivery.
- [ ] Coroutine tests use mock scheduler + direct awaiter construction by default.
- [ ] Fault tests use fixed schedules with known seeds.
- [ ] Lock-free or stress tests have a deterministic oracle or dedicated
  sanitizer/model-checker rationale.

### Verification
- [ ] The verification command is the narrowest relevant test binary or CTest
  pattern, with broader verification only when risk requires it.
