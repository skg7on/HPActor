# EdgeOps Telemetry Platform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the EdgeOps Telemetry Platform example app and deterministic tests that validate HPActor actor lifetime, scheduling, memory/backpressure, message communication, observability, and operator workflows.

**Architecture:** Helper headers define message codecs, scenarios, rollups, and alert rules. `scenario.cpp` owns the all-in-one actor pipeline and returns a deterministic `ScenarioSummary`. `apps/edgeops_telemetry/14_edgeops_telemetry.cpp` is a thin CLI wrapper with all-in-one, role, and query modes.

**Tech Stack:** C++20, HPActor actors and ActorSystem, CMake/Ninja, Google Test, existing dead-letter queue, metrics/tracing/logging config, and example-local hand-rolled binary codecs.

**Design Spec:** `docs/app/app-edgeops-telemetry-platform-design.md`

---

### Task 1: Helper Contracts

**Files:**
- Create: `apps/edgeops_telemetry/messages.hpp`
- Create: `apps/edgeops_telemetry/scenario.hpp`
- Create: `apps/edgeops_telemetry/rollup.hpp`
- Create: `apps/edgeops_telemetry/alert_rules.hpp`
- Create: `tests/system/apps/test_edgeops_messages.cpp`
- Modify: `tests/system/apps/CMakeLists.txt`

- [ ] Write failing tests for TypeTag values, message round trips, malformed decode rejection, scenario parsing, rollup math, and alert rules.
- [ ] Implement the helper headers without exceptions, RTTI, or new HPActor public APIs.
- [ ] Build and run `test_system_apps` filtered to EdgeOps tests.

### Task 2: All-In-One Actor Scenario Runner

**Files:**
- Create: `apps/edgeops_telemetry/scenario.cpp`
- Create: `tests/system/test_system_edgeops_telemetry.cpp`
- Modify: `tests/system/CMakeLists.txt`

- [ ] Write failing system tests for happy path, malformed telemetry, overload, missing route, timer rollup, query summary, and graceful drain.
- [ ] Implement `ScenarioRunConfig`, `ScenarioSummary`, and `run_scenario()` in `scenario.cpp`.
- [ ] Use HPActor `ActorSystem`, spawned actors, scheduled rollup messages, bounded mailbox/DLQ configuration, and actor addresses for the all-in-one flow.
- [ ] Build and run `test_system` filtered to EdgeOps tests.

### Task 3: Executable and Role Modes

**Files:**
- Create: `apps/edgeops_telemetry/14_edgeops_telemetry.cpp`
- Modify: `apps/edgeops_telemetry/CMakeLists.txt`

- [ ] Add CLI parsing for `--all-in-one`, `--gateway`, `--processor`, `--storage`, `--ops`, `--device-simulator`, `--query`, `--scenario`, `--devices`, `--rate`, ports, and query selectors.
- [ ] Print operator-oriented summaries for all-in-one and query modes.
- [ ] Implement long-running role entrypoints that construct role-specific `ActorSystem` instances and print endpoint/runbook evidence.
- [ ] Build and smoke-run the executable with representative scenarios.

### Task 4: Documentation and Verification

**Files:**
- Modify: `docs/app/app-edgeops-telemetry-platform-design.md`

- [ ] Keep the detailed spec aligned with implemented scope and manual multi-process validation.
- [ ] Run targeted verification: configure, build `14_edgeops_telemetry`, build `test_system_apps`, build `test_system`, then run EdgeOps-filtered tests.
- [ ] Inspect `git diff` and `git status` from the worktree before handoff.
