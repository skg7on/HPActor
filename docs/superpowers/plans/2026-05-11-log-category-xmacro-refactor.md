# Log Category X-Macro Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `LogCategory` and `LogEventId` enum-to-string/parse conversions to use X-Macros so each enum value is defined once.

**Architecture:** Introduce a `detail/log_macros.hpp` header containing X-Macro tables for both enums. The public header includes it to generate the enum definitions; the `.cpp` file includes it (via the header) to generate `to_string()` and `parse_category()` function bodies. The `kCount` sentinel stays outside the X-Macro table and is handled explicitly.

**Tech Stack:** C++20, no exceptions, no RTTI, CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-05-11-log-category-xmacro-refactor-design.md`

---

## Pre-flight

### Worktree Setup

- [ ] **Step 1: Create git worktree**

```bash
cd /Users/skg7on/Workspace/Projects/HPActor
git worktree add -b feature/issue-87-xmacro-refactor ../hpa-issue-87 main
```

The worktree will be at `/Users/skg7on/Workspace/Projects/hpa-issue-87`.

---

### Task 1: Create X-Macro tables header

**Files:**
- Create: `include/hpactor/log/detail/log_macros.hpp`

- [ ] **Step 1: Create the detail directory and macro header**

```bash
mkdir -p include/hpactor/log/detail
```

- [ ] **Step 2: Write `log_macros.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

// X-Macro tables for LogCategory and LogEventId.
//
// Usage — generate the enum:
//   #define HPACTOR_ENUM_VALUE(name, str) name,
//   HPACTOR_LOG_CATEGORIES(HPACTOR_ENUM_VALUE)
//   #undef HPACTOR_ENUM_VALUE
//
// Usage — to_string switch:
//   #define HPACTOR_CASE(name, str) case LogCategory::name: return str;
//   HPACTOR_LOG_CATEGORIES(HPACTOR_CASE)
//   #undef HPACTOR_CASE
//
// kCount is kept outside the table (sentinel, handled explicitly).

#define HPACTOR_LOG_CATEGORIES(X) \
    X(kActor,       "actor")       \
    X(kActorState,  "actor_state") \
    X(kMailbox,     "mailbox")     \
    X(kScheduler,   "scheduler")   \
    X(kMemory,      "memory")      \
    X(kRegistrar,   "registrar")   \
    X(kDiscovery,   "discovery")   \
    X(kNetwork,     "network")     \
    X(kRpc,         "rpc")         \
    X(kConfig,      "config")      \
    X(kSupervision, "supervision") \
    X(kCli,         "cli")         \
    X(kHttp,        "http")        \
    X(kUser,        "user")

// LogEventId entries carry an explicit integer value.
// Ranges: 1000-1099 actor, 1100-1199 mailbox, 1200-1299 memory,
//         1300-1399 registrar/discovery, 1400-1499 network, 1500-1599 scheduler.

#define HPACTOR_LOG_EVENTS(X)                                            \
    X(kActorSpawned,                1000, "actor_spawned")               \
    X(kActorTerminated,             1001, "actor_terminated")            \
    X(kActorStateTransfer,          1002, "actor_state_transfer")        \
    X(kActorLinkRejected,           1003, "actor_link_rejected")         \
    X(kMailboxDepthHigh,            1100, "mailbox_depth_high")          \
    X(kMailboxHighWatermark,        1101, "mailbox_high_watermark")      \
    X(kMailboxLowWatermarkRecovered,1102, "mailbox_low_watermark_recovered") \
    X(kMailboxFull,                 1103, "mailbox_full")                \
    X(kMailboxMessageRejected,      1104, "mailbox_message_rejected")    \
    X(kMailboxMessageDropped,       1105, "mailbox_message_dropped")     \
    X(kMailboxOverflowRerouted,     1106, "mailbox_overflow_rerouted")   \
    X(kBackpressureSignalSent,      1107, "backpressure_signal_sent")    \
    X(kSystemReserveExhausted,      1108, "system_reserve_exhausted")    \
    X(kDeadLetterQueued,            1109, "dead_letter_queued")          \
    X(kDeadLetterLost,              1110, "dead_letter_lost")            \
    X(kMemoryAlloc,                 1200, "memory_alloc")                \
    X(kMemoryFree,                  1201, "memory_free")                 \
    X(kMemoryCorruption,            1202, "memory_corruption")           \
    X(kRegistrarRegister,           1300, "registrar_register")          \
    X(kRegistrarResolveMiss,        1301, "registrar_resolve_miss")      \
    X(kDiscoveryNodeJoined,         1302, "discovery_node_joined")       \
    X(kDiscoveryNodeDead,           1303, "discovery_node_dead")         \
    X(kNetworkFrameReceived,        1400, "network_frame_received")      \
    X(kNetworkFrameDecodeFailed,    1401, "network_frame_decode_failed") \
    X(kSchedulerDispatch,           1500, "scheduler_dispatch")          \
    X(kSchedulerSteal,              1501, "scheduler_steal")
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/log/detail/log_macros.hpp
git commit -m "feat(log): add X-Macro tables for LogCategory and LogEventId"
```

---

### Task 2: Refactor header to use X-Macros for enum generation

**Files:**
- Modify: `include/hpactor/log/log_category.hpp:21-71`

- [ ] **Step 1: Replace enum definitions with X-Macro expansion**

Replace lines 21-71 (both enum definitions) with:

```cpp
#include <hpactor/log/detail/log_macros.hpp>

enum class LogCategory : uint16_t {
#define HPACTOR_LOG_CATEGORY_ENUM(name, str) name,
    HPACTOR_LOG_CATEGORIES(HPACTOR_LOG_CATEGORY_ENUM)
#undef HPACTOR_LOG_CATEGORY_ENUM
    kCount, // sentinel, not a valid emit category
};

// Stable numeric IDs for common framework events.
// Ranges: 1000-1099 actor, 1100-1199 mailbox, 1200-1299 memory,
//         1300-1399 registrar/discovery, 1400-1499 network, 1500-1599 scheduler
enum class LogEventId : uint32_t {
#define HPACTOR_LOG_EVENT_ENUM(name, value, str) name = value,
    HPACTOR_LOG_EVENTS(HPACTOR_LOG_EVENT_ENUM)
#undef HPACTOR_LOG_EVENT_ENUM
};
```

- [ ] **Step 2: Verify enum values are identical**

The `LogEventId` entries must produce the same numeric values as before (enums auto-increment from the last explicit value, same as original). Check the generated values are unchanged — each entry carries its original explicit value.

- [ ] **Step 3: Build to verify header compiles**

```bash
cmake -S . -B build -GNinja && ninja -C build
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/log/log_category.hpp
git commit -m "refactor(log): generate LogCategory and LogEventId enums from X-Macros"
```

---

### Task 3: Refactor .cpp to use X-Macros for function bodies

**Files:**
- Modify: `src/log/log_category.cpp`

- [ ] **Step 1: Replace the entire .cpp**

Replace the contents after the `#include` line with X-Macro generated functions:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/log/log_category.hpp>

namespace hpactor::log {

[[nodiscard]] const char* to_string(LogCategory category) noexcept {
    switch (category) {
#define HPACTOR_LOG_CATEGORY_TO_STRING(name, str) case LogCategory::name: return str;
        HPACTOR_LOG_CATEGORIES(HPACTOR_LOG_CATEGORY_TO_STRING)
#undef HPACTOR_LOG_CATEGORY_TO_STRING
        case LogCategory::kCount:
            return "count";
    }
    return "unknown";
}

[[nodiscard]] const char* to_string(LogEventId id) noexcept {
    switch (id) {
#define HPACTOR_LOG_EVENT_TO_STRING(name, value, str) case LogEventId::name: return str;
        HPACTOR_LOG_EVENTS(HPACTOR_LOG_EVENT_TO_STRING)
#undef HPACTOR_LOG_EVENT_TO_STRING
    }
    return "unknown_event";
}

[[nodiscard]] result<LogCategory> parse_category(std::string_view value) noexcept {
#define HPACTOR_LOG_CATEGORY_PARSE(name, str) if (value == str) return result<LogCategory>::make(LogCategory::name);
    HPACTOR_LOG_CATEGORIES(HPACTOR_LOG_CATEGORY_PARSE)
#undef HPACTOR_LOG_CATEGORY_PARSE
    return result<LogCategory>::make(error(errors::unknown, "unknown log category"));
}

} // namespace hpactor::log
```

- [ ] **Step 2: Build and verify**

```bash
ninja -C build
```

Expected: clean build, no warnings.

- [ ] **Step 3: Run existing tests**

```bash
ctest --test-dir build --output-on-failure
```

Expected: all tests pass, including any that exercise log category/event to_string.

- [ ] **Step 4: Commit**

```bash
git add src/log/log_category.cpp
git commit -m "refactor(log): generate to_string/parse_category from X-Macros"
```

---

### Task 4: Final verification

- [ ] **Step 1: Full build with both gcc and clang (if available)**

```bash
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Debug && ninja -C build
ctest --test-dir build --output-on-failure
```

- [ ] **Step 2: Check git log**

```bash
git log --oneline main..HEAD
```

Expected: 3 commits on the feature branch.

- [ ] **Step 3: Verify diff summary vs main**

```bash
git diff main..HEAD --stat
```

Expected: 3 files changed, significant reduction in .cpp line count.

---

## Review Checklist

Before declaring done, verify:
- [ ] `LogCategory` enum values are unchanged (kActor=0 through kUser=13, kCount=14)
- [ ] `LogEventId` numeric values are unchanged (kActorSpawned=1000 through kSchedulerSteal=1501)
- [ ] `to_string(LogCategory::kCount)` still returns `"count"`
- [ ] `parse_category("count")` does NOT parse (returns error)
- [ ] `to_string(LogEventId::kActorSpawned)` returns `"actor_spawned"`
- [ ] All existing tests pass
- [ ] No warnings in Debug or Release builds
