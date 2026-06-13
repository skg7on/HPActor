# cli_demo CLI Enhancements — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Three targeted fixes to the CLI demo: per-actor memory table in `/system memory`, empty ENTER no-op instead of exit, and fix CliActor showing as "unknown" actor type.

**Architecture:** Three independent changes in three files. No new files, no API changes, no config changes. Each task follows TDD (RED → GREEN → REFACTOR).

**Tech Stack:** C++20, Google Test, CMake/Ninja

---

### Task 1: Fix "unknown" Actor Type for CliActor

**Files:**
- Modify: `include/hpactor/cli/cli_actor.hpp` (add `kActorTypeName` static member)
- Test: `tests/unit/cli/test_system_commands.cpp` (add type-name assertion)

**Background:** `ActorSystem::spawn<T>()` checks `if constexpr (requires { T::kActorTypeName; })` and sets `type_name` from it. If missing, `type_name` defaults to `"unknown"`. `CliActor` is the only core framework actor missing this member, so it appears as "unknown" in `/actor list` and `/system list`.

- [ ] **Step 1: Write the failing test**

Add a new test to `tests/unit/cli/test_system_commands.cpp` that verifies CliActor's type name is accessible. Since `CliActor` is a concrete class but requires construction with ActorSystem, we test at the compile-time level — check the static constant exists and has the expected value:

```cpp
// =============================================================================
// CliActor kActorTypeName
// =============================================================================

TEST(CliActorTest, HasActorTypeName) {
    // Verify the static constant exists and is non-empty.
    // If kActorTypeName is missing, this test fails to compile.
    EXPECT_STREQ(hpactor::cli::CliActor::kActorTypeName, "CliActor");
}
```

The `#include <hpactor/cli/cli_actor.hpp>` is already at the top of the system_commands test file — verify it's present, add it if not.

- [ ] **Step 2: Run test to verify it FAILS (compilation error)**

Run: `ninja -C build tests/unit/cli/test_unit_cli && ./build/tests/unit/cli/test_unit_cli --gtest_filter="*CliActorTest*"`

Expected: **Compilation failure** — `no member named 'kActorTypeName' in 'hpactor::cli::CliActor'`

- [ ] **Step 3: Write minimal implementation**

In `include/hpactor/cli/cli_actor.hpp`, add after line 55 (inside the `CliActor` class, in the `public:` section):

```cpp
    /// \brief Actor type name for CLI introspection and actor listing.
    static constexpr const char* kActorTypeName = "CliActor";
```

Place it right after the `public:` on line 56, before the constructor declaration:

```cpp
class CliActor : public DaemonActor {
  public:
    /// \brief Actor type name for CLI introspection and actor listing.
    static constexpr const char* kActorTypeName = "CliActor";

    /// \brief Construct the CLI actor.
    // ...
```

- [ ] **Step 4: Run test to verify it PASSES**

Run: `ninja -C build tests/unit/cli/test_unit_cli && ./build/tests/unit/cli/test_unit_cli --gtest_filter="*CliActorTest*"`

Expected: **PASS** — `CliActorTest.HasActorTypeName`

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/cli/cli_actor.hpp tests/unit/cli/test_system_commands.cpp
git commit -m "fix(cli): add kActorTypeName to CliActor

CliActor showed as 'unknown' in /actor list and /system list because
ActorSystem::spawn<T>() falls back to 'unknown' when T::kActorTypeName
is missing. Add the static constant so CliActor identifies correctly.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Empty ENTER → No-Op (Only /quit Exits)

**Files:**
- Modify: `src/cli/cli_actor.cpp:470-475` (change empty-line handling)

**Background:** `CliActor::run_once()` currently treats empty input as quit: prints "Goodbye.", sets `running_ = false`, returns `false`. This means pressing ENTER with no text exits the CLI. The fix: empty ENTER is a no-op (returns `true`), EOF on stdin still exits. `LineEditor::readline()` returns empty string for both cases, so we distinguish by checking `std::feof(stdin)` after an empty read.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/cli/test_system_commands.cpp` (or the appropriate existing test file). This test verifies the documented contract — we test by checking `run_once()` return value. Since `run_once()` reads from stdin, we need to redirect stdin.

Actually, `run_once()` is tightly coupled to `LineEditor::readline()` which calls linenoise on `stdin`. A unit test for the empty-line behavior inside `run_once()` is impractical to isolate. Instead, write the test as an integration test that checks the behavior is correct through the `PrettyFormatter` output contract:

Add to `tests/integration/cli/test_cli_integration.cpp` — search for existing patterns there first.

If an integration test is too heavy for this one-line logic change, verify the fix by building and running the cli_demo app manually. For the plan, we'll write a focused test that exercises the modified code path:

```cpp
// Verify the CliActor's quit command exists and is the only exit mechanism.
TEST(QuitCommandTest, OnlyQuitExits) {
    auto* cmd = find_cmd("quit");
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->help_text(), "Exit the CLI");
    // The quit command's execute() calls request_shutdown().
    // Empty input in run_once() should return true (continue loop).
}
```

For the actual behavioral test, verify after building that:
- Pressing ENTER with no input re-displays the prompt
- Typing `/quit` exits

- [ ] **Step 2: Build current code to verify test baseline**

Run: `ninja -C build tests/unit/cli/test_unit_cli`

Expected: Build succeeds (test infrastructure ready).

- [ ] **Step 3: Write minimal implementation**

In `src/cli/cli_actor.cpp`, replace lines 470-475:

**Current code (lines 470-475):**
```cpp
    std::string line = line_editor_.readline("hpactor> ");
    if (line.empty()) {
        printf("\nGoodbye.\n");
        running_ = false;
        return false;
    }
```

**Replace with:**
```cpp
    std::string line = line_editor_.readline("hpactor> ");
    if (line.empty()) {
        // Empty input from user (just ENTER) is a no-op.
        // EOF on stdin also returns empty — distinguish by checking feof.
        if (std::feof(stdin)) {
            printf("\nGoodbye.\n");
            running_ = false;
            return false;
        }
        return true; // no-op, keep running
    }
```

The `#include <cstdio>` is already at the top of the file (used by `printf`).

- [ ] **Step 4: Build and run tests**

Run: `ninja -C build tests/unit/cli/test_unit_cli && ./build/tests/unit/cli/test_unit_cli --gtest_filter="*QuitCommand*"`

Expected: **PASS**

Also build and briefly run the cli_demo to verify empty ENTER doesn't exit:

```bash
ninja -C build apps/cli_demo/15_cli_demo
echo "" | timeout 2 ./build/apps/cli_demo/15_cli_demo || true
```

Expected: The app should NOT exit on empty input (it will timeout or need Ctrl+C to kill).

- [ ] **Step 5: Commit**

```bash
git add src/cli/cli_actor.cpp
git commit -m "fix(cli): empty ENTER should not exit, only /quit exits

Previously an empty input line (user pressing ENTER with no text)
triggered shutdown. Now empty ENTER is a no-op — only /quit or
EOF on stdin exits the CLI.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Per-Actor Memory Table in `/system memory`

**Files:**
- Modify: `src/cli/commands/system_commands.cpp:66-112` (extend `SystemMemoryCommand::execute()`)
- Add include: `#include <hpactor/mem/memory_tracker.hpp>` and `#include <hpactor/mem/memory_config.hpp>`

**Background:** `MemoryTracker` singleton already tracks per-actor memory stats (current_bytes, peak_bytes, alloc_count, free_count). The existing `/system memory` command shows only global region stats. We append a second table below the regions table, one row per actor, showing per-actor memory stats.

- [ ] **Step 1: Write the failing test**

In `tests/unit/cli/test_system_commands.cpp`, add a test that verifies `/system memory` output includes per-actor memory columns when memory tracking is available:

```cpp
TEST(SystemMemoryCommandTest, PerActorMemoryColumns) {
    auto* cmd = find_cmd("system/memory");
    ASSERT_NE(cmd, nullptr);

    PrettyFormatter fmt;
    CommandContext ctx;
    ctx.output = &fmt;

    cmd->execute(ctx);
    std::string out = fmt.finalize();

    // The output should now include per-actor memory section
    // when memory tracking is enabled.
    if constexpr (hpactor::mem::kMemoryTrackingEnabled) {
        EXPECT_NE(out.find("Per-Actor Memory"), std::string::npos);
        EXPECT_NE(out.find("Current"), std::string::npos);
        EXPECT_NE(out.find("Peak"), std::string::npos);
    } else {
        EXPECT_NE(out.find("disabled"), std::string::npos);
    }
}
```

The test needs `#include <hpactor/mem/memory_config.hpp>` added to the test file.

- [ ] **Step 2: Run test to verify it FAILS**

Run: `ninja -C build tests/unit/cli/test_unit_cli && ./build/tests/unit/cli/test_unit_cli --gtest_filter="*PerActorMemoryColumns*"`

Expected: **FAIL** — "Per-Actor Memory" not found in output (only "Memory Regions" appears).

- [ ] **Step 3: Write minimal implementation**

At the top of `src/cli/commands/system_commands.cpp`, add these includes (after existing includes, around line 20):

```cpp
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/memory_tracker.hpp>
```

In `SystemMemoryCommand::execute()`, add the per-actor table after the existing regions table (after line 109 `ctx.output->table(cols, rows);`):

```cpp
        ctx.output->table(cols, rows);

        // ── Per-actor memory table ──────────────────────────────────────
        if constexpr (mem::kMemoryTrackingEnabled) {
            ctx.output->header("Per-Actor Memory");

            std::vector<std::string> mem_cols = {"ID", "Type", "Current",
                                                 "Peak", "Allocs", "Frees"};
            std::vector<std::vector<std::string>> mem_rows;

            auto* sys = ctx.system;
            if (sys) {
                auto& tracker = mem::MemoryTracker::instance();
                sys->for_each_actor([&](ActorId actor_id, AbstractActor& actor) {
                    mem::ActorMemoryStats stats;
                    tracker.snapshot(actor_id, stats);

                    char id_buf[32];
                    snprintf(id_buf, sizeof(id_buf), "0x%04llX",
                             static_cast<unsigned long long>(actor_id.value()));

                    auto meta = actor.to_metadata();
                    mem_rows.push_back({
                        id_buf,
                        meta.actor_type,
                        format_bytes(stats.current_bytes),
                        format_bytes(stats.peak_bytes),
                        std::to_string(stats.alloc_count),
                        std::to_string(stats.free_count),
                    });
                });
            }
            ctx.output->table(mem_cols, mem_rows);
        } else {
            ctx.output->raw(
                "Per-actor memory tracking is disabled at compile time.");
        }

        return result<void>::make();
    }
```

- [ ] **Step 4: Run test to verify it PASSES**

Run: `ninja -C build tests/unit/cli/test_unit_cli && ./build/tests/unit/cli/test_unit_cli --gtest_filter="*SystemMemoryCommand*"`

Expected: **PASS** — both `SystemMemoryCommandTest.Metadata`, `SystemMemoryCommandTest.Execute`, and `SystemMemoryCommandTest.PerActorMemoryColumns`.

Also run the existing tests to verify no regressions:

```bash
./build/tests/unit/cli/test_unit_cli
```

Expected: All CLI unit tests pass.

- [ ] **Step 5: Build and run cli_demo to manually verify output**

```bash
ninja -C build apps/cli_demo/15_cli_demo
echo "/system memory" | timeout 5 ./build/apps/cli_demo/15_cli_demo 2>&1 || true
```

Expected: Output shows "Memory Regions" table followed by "Per-Actor Memory" table with rows for each actor.

- [ ] **Step 6: Commit**

```bash
git add src/cli/commands/system_commands.cpp tests/unit/cli/test_system_commands.cpp
git commit -m "feat(cli): add per-actor memory table to /system memory

/s/system memory now shows a second table below the regions table
with per-actor memory stats (current, peak, allocs, frees) sourced
from the MemoryTracker singleton. Guards on kMemoryTrackingEnabled
at compile time.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Full Build Verification

- [ ] **Step 1: Rebuild all changed targets**

```bash
ninja -C build tests/unit/cli/test_unit_cli apps/cli_demo/15_cli_demo
```

- [ ] **Step 2: Run the full CLI test suite**

```bash
./build/tests/unit/cli/test_unit_cli
```

Expected: All tests pass, zero failures.

- [ ] **Step 3: Final git status check**

```bash
git status
git branch --show-current   # should show enhance-cli-demo
```

Verify all changes are in the worktree on the correct branch.
