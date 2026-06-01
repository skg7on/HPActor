---
name: tddflow-development
description: Use when implementing HPActor features, bug fixes, refactors, or behavior changes after design approval and before writing implementation code.
---

# TDDFlow Development

Use this skill after the design/spec is accepted and before production
implementation begins.

## Core Rule

No production code before a failing test.

For each behavior, edge case, or regression:

1. **RED:** write one focused test that states the desired behavior.
2. Run the narrowest relevant command and confirm the test fails for the
   expected reason, not from a typo, bad fixture, or build error.
3. **GREEN:** implement the smallest code change that can pass that test.
4. Re-run the same focused command and confirm it passes.
5. **REFACTOR:** clean names, duplication, or structure only after green, then
   re-run the focused command.
6. Repeat until the accepted design is implemented.

If implementation was written before RED, delete it and restart from the test.

## HPActor Test Selection

- Prefer the narrowest test binary or CTest pattern that covers the change.
- Add unit tests for local behavior, integration tests for actor/config/network
  interactions, and system or stress tests for reliability-plane behavior.
- Do not reconfigure or rebuild the whole project unless CMake, generated
  protobuf, toolchain, shared public headers, or broad runtime contracts changed.
- Use the worktree-local `build/` directory for every configure, build, and test
  command.

Common focused commands:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build <target>
./build/tests/unit/core/test_unit_core --gtest_filter="Suite.Test"
ctest -R "Pattern" --output-on-failure
```

## Deterministic Tests

- Avoid timing assumptions, sleeps, and assumed scheduler ordering.
- Use `scheduler_threads = 0` when a test must inspect intermediate mailbox,
  lifecycle, or pressure state directly.
- Prefer condition-based polling with generous timeouts when scheduler progress
  is the behavior under test.
- Use real code and observable behavior; mocks are acceptable only when the real
  dependency is impractical or would make the test nondeterministic.

## Exceptions

Ask the user before skipping TDD for generated code, throwaway exploration,
docs-only changes, configuration-only changes, or work where no executable
behavior can be tested. If an exception is approved, still run the narrowest
available verification for the touched files.

## Completion Checklist

- The final response names the RED command and expected failure.
- The final response names the GREEN command and passing result.
- Every new or changed behavior has a test that failed before implementation.
- Focused tests pass after refactor.
- Broader verification was run when the change affected shared runtime behavior.
