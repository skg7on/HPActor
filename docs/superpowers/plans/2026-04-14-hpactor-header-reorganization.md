# HPActor Header Reorganization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reorganize flat-organized header files from `include/hpactor/` root into proper architectural subdirectories matching the source file organization (`actor/`, `ref/`, `net/`, `supervision/`, `core/`, `types/`).

**Architecture:** Group header files by architectural role — actor types move to `actor/`, reference types to `ref/`, networking types to `net/`, supervision types to `supervision/`, core runtime infrastructure to `core/`, and type system headers to `types/`. Update all `#include` paths in both headers and source files to reflect new locations.

**Tech Stack:** C++20, CMake

---

## File Structure

### Headers to Move

| File | Current Location | Target Directory |
|------|-----------------|-----------------|
| `mutex_mailbox.hpp` | root | `core/` |
| `platform.hpp` | root | `net/` |
| `scheduler.hpp` | root | `core/` |
| `behavior.hpp` | root | `actor/` |
| `typed_behavior.hpp` | root | `actor/` |
| `mailbox.hpp` | root | `core/` |
| `message.hpp` | root | `actor/` |
| `types.hpp` | root | `types/` |
| `types_fwd.hpp` | root | `types/` |
| `serialization.hpp` | root | `types/` |
| `actor_system.hpp` | root | `core/` |
| `actor_system_ids.hpp` | root | `core/` |
| `actor_registry.hpp` | root | `core/` |
| `actor_type_registry.hpp` | root | `actor/` |
| `spawn.hpp` | root | `actor/` |
| `actor_context.hpp` | root | `actor/` (ALREADY in actor/) |
| `actor_address.hpp` | root | `ref/` (ALREADY in ref/) |

### Subdirectories to Create

- `include/hpactor/core/` — core runtime infrastructure
- `include/hpactor/types/` — type system headers

### Files to Update (include paths)

**Headers updating other headers:**
- `actor/actor_context.hpp` — include `core/mailbox.hpp`
- `actor/abstract_actor.hpp` — include `core/mailbox.hpp`, `actor/message.hpp`
- `actor/local_actor.hpp` — include `actor/message.hpp`
- `actor/event_based_actor.hpp` — include `actor/message.hpp`
- `actor/stateful_actor.hpp` — include `actor/message.hpp`
- `actor/blocking_actor.hpp` — include `actor/message.hpp`
- `actor/typed_actor.hpp` — include `actor/message.hpp`
- `actor/spawn_receiver.hpp` — include `actor/message.hpp`
- `behavior.hpp` → `actor/behavior.hpp`
- `typed_behavior.hpp` → `actor/typed_behavior.hpp`
- `supervision/supervision.hpp` — include `actor/message.hpp`
- `supervision/one_for_one_supervisor.hpp` — include `actor/message.hpp`
- `supervision/all_for_one_supervisor.hpp` — include `actor/message.hpp`

**Source files updating includes:**
- `src/actor/abstract_actor.cpp`
- `src/actor/local_actor.cpp`
- `src/actor/event_based_actor.cpp`
- `src/actor/scheduler.cpp`
- `src/actor/actor_context.cpp`
- `src/actor/spawn_receiver.cpp`
- `src/actor/actor_system.cpp`
- `src/supervision/supervision.cpp`
- `src/core/serialization.cpp`

---

## Task 1: Create new subdirectories

**Files:**
- Create: `include/hpactor/core/`
- Create: `include/hpactor/types/`

- [ ] **Step 1: Create directory structure**

```bash
mkdir -p include/hpactor/core
mkdir -p include/hpactor/types
```

---

## Task 2: Move core/ headers (scheduler, mailbox, message, platform, mutex_mailbox)

**Files:**
- Move: `include/hpactor/scheduler.hpp` → `include/hpactor/core/`
- Move: `include/hpactor/mailbox.hpp` → `include/hpactor/core/`
- Move: `include/hpactor/mutex_mailbox.hpp` → `include/hpactor/core/`
- Move: `include/hpactor/message.hpp` → `include/hpactor/actor/`
- Move: `include/hpactor/platform.hpp` → `include/hpactor/net/`
- Move: `include/hpactor/actor_system_ids.hpp` → `include/hpactor/core/`
- Move: `include/hpactor/actor_registry.hpp` → `include/hpactor/core/`
- Move: `include/hpactor/actor_system.hpp` → `include/hpactor/core/`

- [ ] **Step 1: Move scheduler.hpp**

```bash
mv include/hpactor/scheduler.hpp include/hpactor/core/
```

- [ ] **Step 2: Move mailbox.hpp and mutex_mailbox.hpp**

```bash
mv include/hpactor/mailbox.hpp include/hpactor/core/
mv include/hpactor/mutex_mailbox.hpp include/hpactor/core/
```

- [ ] **Step 3: Move message.hpp**

```bash
mv include/hpactor/message.hpp include/hpactor/actor/
```

- [ ] **Step 4: Move platform.hpp**

```bash
mv include/hpactor/platform.hpp include/hpactor/net/
```

- [ ] **Step 5: Move actor_system*.hpp and actor_registry.hpp**

```bash
mv include/hpactor/actor_system_ids.hpp include/hpactor/core/
mv include/hpactor/actor_registry.hpp include/hpactor/core/
mv include/hpactor/actor_system.hpp include/hpactor/core/
```

---

## Task 3: Move actor/ headers (behavior, typed_behavior, spawn, actor_type_registry)

**Files:**
- Move: `include/hpactor/behavior.hpp` → `include/hpactor/actor/`
- Move: `include/hpactor/typed_behavior.hpp` → `include/hpactor/actor/`
- Move: `include/hpactor/spawn.hpp` → `include/hpactor/actor/`
- Move: `include/hpactor/actor_type_registry.hpp` → `include/hpactor/actor/`

- [ ] **Step 1: Move behavior.hpp and typed_behavior.hpp**

```bash
mv include/hpactor/behavior.hpp include/hpactor/actor/
mv include/hpactor/typed_behavior.hpp include/hpactor/actor/
```

- [ ] **Step 2: Move spawn.hpp and actor_type_registry.hpp**

```bash
mv include/hpactor/spawn.hpp include/hpactor/actor/
mv include/hpactor/actor_type_registry.hpp include/hpactor/actor/
```

---

## Task 4: Move types/ headers (types, types_fwd, serialization)

**Files:**
- Move: `include/hpactor/types.hpp` → `include/hpactor/types/`
- Move: `include/hpactor/types_fwd.hpp` → `include/hpactor/types/`
- Move: `include/hpactor/serialization.hpp` → `include/hpactor/types/`

- [ ] **Step 1: Move type system headers**

```bash
mv include/hpactor/types.hpp include/hpactor/types/
mv include/hpactor/types_fwd.hpp include/hpactor/types/
mv include/hpactor/serialization.hpp include/hpactor/types/
```

---

## Task 5: Update include paths in actor/ headers

**Files:**
- Modify: `include/hpactor/actor/actor_context.hpp`
- Modify: `include/hpactor/actor/abstract_actor.hpp`
- Modify: `include/hpactor/actor/local_actor.hpp`
- Modify: `include/hpactor/actor/event_based_actor.hpp`
- Modify: `include/hpactor/actor/stateful_actor.hpp`
- Modify: `include/hpactor/actor/blocking_actor.hpp`
- Modify: `include/hpactor/actor/typed_actor.hpp`
- Modify: `include/hpactor/actor/spawn_receiver.hpp`
- Modify: `include/hpactor/actor/behavior.hpp`
- Modify: `include/hpactor/actor/typed_behavior.hpp`

- [ ] **Step 1: Update actor_context.hpp includes**

Update from: `#include "mailbox.hpp"`, `#include "message.hpp"`
To: `#include "core/mailbox.hpp"`, `#include "actor/message.hpp"`

- [ ] **Step 2: Update abstract_actor.hpp includes**

Update from: `#include "mailbox.hpp"`, `#include "message.hpp"`
To: `#include "core/mailbox.hpp"`, `#include "actor/message.hpp"`

- [ ] **Step 3: Update local_actor.hpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 4: Update event_based_actor.hpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 5: Update stateful_actor.hpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 6: Update blocking_actor.hpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 7: Update typed_actor.hpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 8: Update spawn_receiver.hpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 9: Update behavior.hpp (moved) — no changes needed if includes use relative paths**

- [ ] **Step 10: Update typed_behavior.hpp (moved) — no changes needed if includes use relative paths**

---

## Task 6: Update include paths in supervision/ headers

**Files:**
- Modify: `include/hpactor/supervision/supervision.hpp`
- Modify: `include/hpactor/supervision/one_for_one_supervisor.hpp`
- Modify: `include/hpactor/supervision/all_for_one_supervisor.hpp`

- [ ] **Step 1: Update supervision.hpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 2: Update one_for_one_supervisor.hpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 3: Update all_for_one_supervisor.hpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

---

## Task 7: Update include paths in src/ source files

**Files:**
- Modify: `src/actor/abstract_actor.cpp`
- Modify: `src/actor/local_actor.cpp`
- Modify: `src/actor/event_based_actor.cpp`
- Modify: `src/actor/scheduler.cpp`
- Modify: `src/actor/actor_context.cpp`
- Modify: `src/actor/spawn_receiver.cpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `src/supervision/supervision.cpp`
- Modify: `src/core/serialization.cpp`

- [ ] **Step 1: Update abstract_actor.cpp includes**

Update from: `#include "mailbox.hpp"`, `#include "message.hpp"`
To: `#include "core/mailbox.hpp"`, `#include "actor/message.hpp"`

- [ ] **Step 2: Update local_actor.cpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 3: Update event_based_actor.cpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 4: Update scheduler.cpp includes**

Update from: `#include "scheduler.hpp"`
To: `#include "core/scheduler.hpp"`

- [ ] **Step 5: Update actor_context.cpp includes**

Update from: `#include "mailbox.hpp"`, `#include "message.hpp"`
To: `#include "core/mailbox.hpp"`, `#include "actor/message.hpp"`

- [ ] **Step 6: Update spawn_receiver.cpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 7: Update actor_system.cpp includes**

Update from: `#include "actor_system.hpp"`
To: `#include "core/actor_system.hpp"`

- [ ] **Step 8: Update supervision/supervision.cpp includes**

Update from: `#include "message.hpp"`
To: `#include "actor/message.hpp"`

- [ ] **Step 9: Update core/serialization.cpp includes**

Update from: `#include "serialization.hpp"`
To: `#include "types/serialization.hpp"`

---

## Task 8: Update net/ headers (platform.hpp moved)

**Files:**
- Modify: `include/hpactor/net/gcd_backend.hpp`
- Modify: `include/hpactor/net/iouring_backend.hpp`
- Modify: `include/hpactor/net/event_loop.hpp`
- Modify: `include/hpactor/net/async_io_backend.hpp`

- [ ] **Step 1: Update gcd_backend.hpp includes**

Update from: `#include "platform.hpp"`
To: `#include "platform.hpp"` (path unchanged since platform.hpp already in net/)

- [ ] **Step 2: Update iouring_backend.hpp includes**

Update from: `#include "platform.hpp"`
To: `#include "platform.hpp"` (path unchanged since platform.hpp already in net/)

- [ ] **Step 3: Update event_loop.hpp includes**

Update from: `#include "platform.hpp"`
To: `#include "platform.hpp"` (path unchanged since platform.hpp already in net/)

- [ ] **Step 4: Update async_io_backend.hpp includes**

Update from: `#include "platform.hpp"`
To: `#include "platform.hpp"` (path unchanged since platform.hpp already in net/)

**Note:** Since platform.hpp was flat at root level but used by net/ headers, it needed to move to net/. Headers inside net/ that already had `#include "platform.hpp"` need no changes since platform.hpp is now in the same directory.

---

## Task 9: Verify build

- [ ] **Step 1: Run CMake configure and build**

```bash
cmake -S . -B build -GNinja
ninja -C build
```

Expected: Build completes without include errors

- [ ] **Step 2: Run tests**

```bash
ctest --output-on-failure
```

Expected: All tests pass

---

## Task 10: Commit

- [ ] **Step 1: Stage all changes**

```bash
git add -A
```

- [ ] **Step 2: Commit**

```bash
git commit -m "refactor: reorganize headers into architectural subdirectories

Move flat root-level headers into proper architectural groups:
- actor/: behavior, typed_behavior, spawn, actor_type_registry, message
- core/: scheduler, mailbox, mutex_mailbox, actor_system, actor_system_ids, actor_registry
- types/: types, types_fwd, serialization
- net/: platform

Update all include paths in headers and source files to reflect new locations."
```
