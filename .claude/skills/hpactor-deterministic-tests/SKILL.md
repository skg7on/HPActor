---
name: hpactor-deterministic-tests
description: Use when adding, changing, reviewing, or debugging HPActor tests that are flaky, timing-sensitive, OS-dependent, CI-only failures, thread-scheduling-dependent, timer-based, scheduler-driven, mailbox/actor-state inspecting, or concurrent stress tests.
---

# HPActor Deterministic Tests

Design HPActor tests so the result does not depend on wall-clock timing, OS
thread scheduling, CPU speed, macOS/Linux differences, or repeated reruns.

## Required Reads

Read the repo warmup files first: `AGENTS.md`, `CLAUDE.md`,
`CLAUDE_MEMORY.md`, and `HPACTOR_PROJECT_OUTLINE.md`.

For actor, scheduler, mailbox, timer, or concurrency tests, also read:

- `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- `tests/support/system_test_fixture.hpp`
- `tests/support/scheduler_test_driver.hpp`
- Existing tests in the same tier and subsystem.

Work in the required `.worktrees/` checkout before writing or changing tests.

## Lessons From Past Flakes

Use these PRs as failure examples:

- PR #112: timer tests passed locally but failed in CI because timer delivery
  depended on timer-thread startup and wall-clock delay. Fixes separated API
  behavior from real timer delivery, used `scheduler_threads = 0`, inspected
  mailboxes directly, and relaxed assertions on state that can advance on a
  timer thread before the test observes it.
- PR #117: a concurrent stress test failed on gcc/Linux because success
  depended on thread interleaving and also exposed alignment UB. Fixes replaced
  the stress test with deterministic single-threaded write/read-back coverage
  and used test assertions that remain active in all build modes.
- PR #142: tests timed out or crashed because they depended on kernel worker
  scheduling and fed fake actor IDs to real workers. Fixes used
  `scheduler_start_paused`, `SchedulerTestDriver`, `pin_actor_to_worker()`,
  `run_actor()`, `run_one_on_worker()`, `drain_ready()`, and
  `scheduler_threads = 0` for tests that inspect state.

## Design Workflow

### 1. Classify The Async Surface

Before writing assertions, state which async source the test touches:

- No async runtime: pure unit behavior.
- Actor delivery or mailbox state.
- Cooperative scheduler worker execution.
- Timer thread or scheduled delivery.
- Dedicated-thread actors (`BlockingActor`, `DaemonActor`) or real OS threads.
- Network/event-loop callbacks.
- Lock-free structure correctness or stress/race behavior.

If the test does not intentionally verify the async source itself, remove that
source from the test.

### 2. Pick The Deterministic Control

Use the narrowest deterministic surface:

- Pure object behavior: construct the object directly; no `ActorSystem`.
- Mailbox/admission/state inspection: `Config cfg; cfg.scheduler_threads = 0;`
  then inspect mailbox or actor state before any worker can drain it.
- Actor processing: set `scheduler_start_paused = true`, create
  `hpactor::test::SchedulerTestDriver`, then drive execution with
  `run_one()`, `drain()`, or `drain_until()`.
- Specific actor or worker ordering: use `pin_actor_to_worker()`,
  `run_actor()`, and `run_one_on_worker()`.
- Timer arithmetic/data structure behavior: prefer direct timer-backend unit
  tests with explicit advancement or controlled state.
- Timer API behavior: assert stable API outcomes such as valid handles, safe
  invalid cancel, idempotent cancel, and mailbox state after cancellation.
- End-to-end timer delivery, network progress, or dedicated-thread behavior:
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

- `sleep_for()` is used to make actor/scheduler/timer progress happen.
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
runtime path, not a unit API, is the behavior under test.

### Replace Interleaving Stress With Deterministic Invariants

Bad: many threads loop thousands of times and only "pass if no crash".

Good: build a deterministic oracle. Acquire all resources, write sentinels,
release them, reacquire them, and verify identities, bounds, and preserved
contents. Add sanitizer, Relacy, or dedicated race tests only when changing the
lock-free algorithm itself.

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

## Review Checklist

Before accepting a test:

- The test name states one behavior.
- The test uses the narrowest tier: unit before integration before system.
- No assertion depends on wall-clock delay, worker scheduling, or platform
  thread ordering unless that behavior is explicitly under test.
- Scheduler tests use `scheduler_threads = 0`, `scheduler_start_paused`, or
  `SchedulerTestDriver` when possible.
- Timer tests separate API semantics from real delivery.
- Lock-free or stress tests have a deterministic oracle or dedicated
  sanitizer/model-checker rationale.
- The verification command is the narrowest relevant test binary or CTest
  pattern, with broader verification only when risk requires it.
