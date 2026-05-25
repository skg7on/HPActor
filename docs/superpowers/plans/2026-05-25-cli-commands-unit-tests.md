# Implementation Plan: CLI Commands Unit Tests

**Date:** 2026-05-25
**Branch:** `worktree-cli-commands-unit-tests`
**Goal:** Add comprehensive unit tests for `src/cli/commands/` submodule (currently 0% direct coverage)

## Summary

Create 6 new test source files in `tests/unit/cli/` covering all 15 commands and the `parse_actor_id()` utility. Total: ~520 lines of test code. All commands exercise metadata (path/help_text/order) plus execute() branches reachable without a running ActorSystem.

## Step 1: `test_command_utils.cpp` — `parse_actor_id()`

**File:** `tests/unit/cli/test_command_utils.cpp`

Include `command_utils.hpp` directly. Test cases:

| Test | Input | Expected |
|------|-------|----------|
| ParseDecimal | "123" | ActorId{123} |
| ParseHexLower | "0xABCD" | ActorId{0xABCD} |
| ParseHexUpper | "0XDEAD" | ActorId{0xDEAD} |
| ParseMaxHex | "0xFFFFFFFFFFFFFFFF" | ActorId{max} |
| ParseZero | "0" | ActorId{0} |
| ParseHexZero | "0x0" | ActorId{0} |
| ParseEmptyString | "" | ActorId{0} |
| ParseNonNumeric | "garbage" | ActorId{0} |
| ParsePartialHex | "0xGHIJ" | ActorId{0} |
| ParseLeadingWhitespace | " 123" | ActorId{0} |

## Step 2: `test_failure_commands.cpp` — FailureReasons + FailureSummary

**File:** `tests/unit/cli/test_failure_commands.cpp`

Include `failure_commands.cpp` internals. Create command instances, exercise:

- `FailureReasonsCommand`:
  - `path()` returns "failure/reasons"
  - `help_text()` non-empty
  - `order()` returns 600
  - `execute()` → table output with "Reason"/"Code"/"Retryable" columns, 23 rows
  - Verify specific reason strings appear in output
- `FailureSummaryCommand`:
  - `path()` returns "failure/summary"
  - `execute()` → key-value output with expected keys present

## Step 3: `test_misc_commands.cpp` — MetricsShow + TopologyShow

**File:** `tests/unit/cli/test_misc_commands.cpp`

Include `misc_commands.cpp` internals. Both are stub commands:

- Each: verify `path()`, `help_text()`, `order()`
- `MetricsShowCommand::execute()` → "not yet implemented" in output
- `TopologyShowCommand::execute()` → "not yet implemented" in output

## Step 4: `test_help_quit_commands.cpp` — HelpCommand + QuitCommand

**File:** `tests/unit/cli/test_help_quit_commands.cpp`

Create a lightweight fixture with a `CommandNode` for help output:

- `HelpCommand`:
  - `path()` = "help", `order()` = 0
  - execute with cli_actor + command_tree → output contains "Available Commands" and command tree help text
  - execute with null cli_actor → no crash, graceful output (no command tree help)
- `QuitCommand`:
  - `path()` = "quit", `order()` = 9999
  - execute with cli_actor → output "Goodbye."
  - execute with null cli_actor → no crash, still prints "Goodbye."

## Step 5: `test_actor_commands.cpp` — ActorShow, ActorKill, ActorList

**File:** `tests/unit/cli/test_actor_commands.cpp`

Use `PrettyFormatter` for output capture. Test the validation and error branches:

- `ActorShowCommand`:
  - Missing `<id>` param → error "Missing actor ID"
  - Invalid ID ("garbage") → error "Invalid actor ID"
  - Null cli_actor → error "Internal error: no CLI actor"
  - Metadata: path="actor/<id>/show", order=100
- `ActorKillCommand`:
  - Missing `<id>` param → error "Missing actor ID"
  - Invalid ID → error "Invalid actor ID"
  - Null cli_actor → error "Internal error: no CLI actor"
  - Metadata: path="actor/<id>/kill", order=200
- `ActorListCommand`:
  - Null cli_actor → error "Internal error: no CLI actor"
  - Has optional --filter flag (parsing verified)
  - Metadata: path="actor/list", order=300

## Step 6: `test_system_commands.cpp` — System* commands

**File:** `tests/unit/cli/test_system_commands.cpp`

- `SystemStatsCommand`:
  - Null system → error "Internal error: no actor system"
  - Metadata: path="system/stats", order=100
- `SystemMemoryCommand`:
  - Happy path (pure formatter) → "Memory subsystem active" in output
  - Metadata: path="system/memory", order=200
- `SystemListCommand`:
  - Null system → error "Internal error: no actor system"
  - Metadata: path="system/list", order=300
- `SystemDrainCommand`:
  - Null system → error "Internal error: no actor system"
  - Metadata: path="system/drain", order=400
- `SystemDrainStatusCommand`:
  - Null system → error "Internal error: no actor system"
  - Metadata: path="system/drain/status", order=410
- `SystemStopCommand`:
  - Missing `<actor_id>` → error "Missing actor ID"
  - Invalid actor ID → error "Invalid actor ID"
  - Null system/cli → error "Internal error"
  - --force flag → params captured
  - Metadata: path="system/stop/<actor_id>", order=500

## Step 7: CMakeLists.txt — Register new test source

**File:** `tests/unit/cli/CMakeLists.txt`

Add 6 new `.cpp` files to the existing `test_unit_cli` target.

## Step 8: Build and verify

```bash
ninja -C build
ctest --test-dir build --output-on-failure --parallel 8
```

All existing 1066 tests must continue to pass. New tests must all pass.

## Files Changed

| File | Action | Lines |
|------|--------|-------|
| `tests/unit/cli/test_command_utils.cpp` | Create | ~55 |
| `tests/unit/cli/test_failure_commands.cpp` | Create | ~90 |
| `tests/unit/cli/test_misc_commands.cpp` | Create | ~50 |
| `tests/unit/cli/test_help_quit_commands.cpp` | Create | ~80 |
| `tests/unit/cli/test_actor_commands.cpp` | Create | ~140 |
| `tests/unit/cli/test_system_commands.cpp` | Create | ~170 |
| `tests/unit/cli/CMakeLists.txt` | Edit | ~6 added |
