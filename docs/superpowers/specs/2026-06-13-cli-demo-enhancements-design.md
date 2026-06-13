# cli_demo CLI Enhancements — Design

Date: 2026-06-13

## Motivation

Three targeted improvements to the `apps/cli_demo` interactive CLI experience:

1. `/system memory` should show per-actor memory stats, not only global region aggregates
2. Empty ENTER (no input) should be a no-op, not exit the CLI
3. An "unknown" actor type appears in `/actor list` — the `CliActor` itself

---

## Feature 1: Per-Actor Memory Table in `/system memory`

### Current behavior

`/system memory` (`src/cli/commands/system_commands.cpp`, class `SystemMemoryCommand`)
iterates the 6 `RegionType` values and prints a table of global region stats
(active bytes, limit, pressure, alloc/free counts, corruption events).

### New behavior

Append a second table below the existing regions table, with one row per actor:

| Column    | Source                                    |
|-----------|-------------------------------------------|
| ID        | `actor_id.value()` (hex, same as `/system list`) |
| Type      | `actor.to_metadata().actor_type`          |
| Current   | `ActorMemoryStats::current_bytes`         |
| Peak      | `ActorMemoryStats::peak_bytes`            |
| Allocs    | `ActorMemoryStats::alloc_count`           |
| Frees     | `ActorMemoryStats::free_count`            |

Data comes from `mem::MemoryTracker::instance().snapshot(actor_id, stats)`.

### Guard

Wrap in `if constexpr (mem::kMemoryTrackingEnabled)`. If compiled out
(`ENABLE_MEMORY_TRACKING=OFF`), show a note: "Per-actor memory tracking
is disabled at compile time."

### Bytes formatting

Reuse the existing `format_bytes()` helper in `command_utils.cpp`.

### Implementation

Modify `SystemMemoryCommand::execute()` in `src/cli/commands/system_commands.cpp`.
No new files. ~15 lines added.

---

## Feature 2: Empty ENTER → No-Op

### Current behavior

`CliActor::run_once()` in `src/cli/cli_actor.cpp`:

```cpp
std::string line = line_editor_.readline("hpactor> ");
if (line.empty()) {
    printf("\nGoodbye.\n");
    running_ = false;
    return false;
}
```

Empty input prints "Goodbye." and terminates the CLI loop.

### New behavior

Empty input is silently ignored — return `true` to continue the loop.
Only `/quit` (or EOF, which `readline` returns as empty when the stream closes)
exits the CLI.

### EOF vs empty ENTER

`LineEditor::readline` returns an empty string in two cases:
1. User presses ENTER with no text
2. EOF on stdin

To distinguish, check `stdin` state after empty read. If `std::feof(stdin)`,
treat as quit. Otherwise, no-op.

### Implementation

Modify `CliActor::run_once()` in `src/cli/cli_actor.cpp`. ~5 lines changed.

---

## Feature 3: Fix "unknown" Actor Type

### Root cause

`ActorSystem::spawn<T>()` in `include/hpactor/core/actor_system.hpp`:

```cpp
if constexpr (requires { T::kActorTypeName; }) {
    actor->set_type_name(T::kActorTypeName);
} else {
    actor->set_type_name("unknown");
}
```

`CliActor` does not define `kActorTypeName`, so its `type_name` defaults to
`"unknown"`, which appears in `/actor list` and `/system list`.

### Fix

Add to `CliActor` in `include/hpactor/cli/cli_actor.hpp`:

```cpp
static constexpr std::string_view kActorTypeName = "CliActor";
```

The `ActorSystem::spawn` template uses `if constexpr (requires { T::kActorTypeName; })`,
so adding this static member is sufficient — no other wiring needed.

### Files changed

| File | Change |
|------|--------|
| `include/hpactor/cli/cli_actor.hpp` | Add `kActorTypeName` static member |

---

## Scope

Three files, ~25 lines total. No new files, no API changes, no config changes.
All changes are backward-compatible.
