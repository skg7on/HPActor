# 6. Testing

**All tests MUST be deterministic** across platforms, build configurations, and
CI environments. The following rules prevent flaky tests.

## Timing & Ordering

- **Never assume timing.** Do not assume a timer fires within N ms, a thread
  completes within a deadline, or a sleep is "long enough." Use condition-based
  polling with generous timeouts (5s+) for scheduler-dependent tests.
- **Never assume thread execution order.** If a test needs to observe
  intermediate mailbox or lifecycle state, set `scheduler_threads = 0` or
  inject messages directly via `mailbox->inject_for_test()`.

## Filesystem & Resource Isolation

- **Never use hardcoded filesystem paths shared across test cases.** When CTest
  runs tests in parallel (`-j N`), each test case is a separate process — if
  they share a temp directory, pidfile, socket path, or any other filesystem
  resource, they race on create / read / write / unlink.
- **Use unique paths per test case.** Derive temp directories from the test name
  (`UnitTest::GetInstance()->current_test_info()->name()`), the process PID, or
  a random suffix. This guarantees parallel processes never collide on the same
  resource.
- **Clean up in `TearDown`.** Each test must remove only its own resources. Use
  `std::filesystem::remove_all` with an `error_code` out-parameter — never throw
  from cleanup, and never rely on `TearDown` ordering across test cases.
- **Never assume process isolation.** Even when `gtest_discover_tests` creates
  one CTest test per GTest case (separate processes), parallel execution means
  they still share the filesystem. A test that is deterministic when run singly
  may fail under `ctest -j`.

## Platform Portability

- Guard platform-specific syscall behavior (e.g., `sendto` on connected sockets,
  `readv` with zero-length buffers, signal delivery in forked children, page
  sizes) with `#ifdef`. Prefer testing the observable outcome rather than a
  specific errno or signal number.
- Use non-blocking file descriptors in async epoll/kqueue tests. Blocking fds
  cause infinite hangs in the edge-triggered drain loop.

## Test Infrastructure

- Never rely on `assert()` for the condition under test — it compiles out under
  `NDEBUG`. Tests must fail explicitly (return non-zero, print FAIL). Use
  `assert` only for invariants that indicate test infrastructure bugs.
- Use `mailbox->inject_for_test()` for mailbox/drain tests to avoid races where
  the scheduler processes messages before the test can observe them.
- Set CMake `TIMEOUT` properties for tests that legitimately need more than
  the global ctest timeout.

## Meaningful Tests Only

**Every test MUST exercise production code.** Tests that only verify language
primitives, arithmetic, or logic expressed entirely in local variables are
worse than useless — they create a false sense of coverage without validating
any actual behavior.

A test is meaningless if:
- It only declares local variables and asserts relationships between them
  (e.g., `uint64_t a = 5; uint64_t b = 3; EXPECT_LE(b, a);`).
- It tests C++ language semantics rather than project code (e.g., "prove
  that subtraction works" or "prove that `==` compares values").
- It calls no function, method, or constructor from the `hpactor::` namespace
  (or the subsystem under test).
- The entire test body could be deleted without changing what is actually
  tested about the production code.

A meaningful unit test MUST:
- Construct, call, or interact with at least one production type or function
  from the subsystem named in the test file.
- Verify observable behavior of that production code — state changes, return
  values, side effects, error paths, or boundary conditions.
- Fail if the production code's contract changes in an incompatible way.

Before writing a test, ask: "If someone broke the production code this test
claims to cover, would this test catch it?" If the answer is no, the test
is meaningless.

## Scope

Match test scope to risk:
- **Unit tests** — local behavior, single component
- **Integration tests** — actor/config/network interactions
- **Sanitizer, stress, chaos, soak, compatibility** — reliability-plane changes
