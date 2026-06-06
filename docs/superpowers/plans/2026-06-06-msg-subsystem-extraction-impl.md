# Msg Subsystem Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract message lifecycle types from `types/`, `actor/`, `mailbox/`, and `net/` into a cohesive `include/hpactor/msg/` header-only subsystem, preserving source compatibility via shims.

**Architecture:** Three-phase migration: (1) create `msg/` headers with canonical definitions + compatibility shims at old paths, (2) update all internal consumers to use `msg/` paths directly, (3) remove shims. No behavior change, no new `.cpp` files, no CMake changes.

**Tech Stack:** C++20, header-only, compatibility shim pattern, Google Test for verification.

**Design spec:** `docs/superpowers/specs/2026-06-06-msg-subsystem-extraction-design.md`

---

## File Structure Map

### New files (create in `include/hpactor/msg/`)

| File | Content source | Namespace |
|------|---------------|-----------|
| `msg/fwd.hpp` | New — forward declarations | `hpactor` + `hpactor::mailbox` |
| `msg/failure_reason.hpp` | `types/failure_reason.hpp` (whole file) | `hpactor` |
| `msg/failure_envelope.hpp` | `types/failure_envelope.hpp` (whole file) | `hpactor` |
| `msg/type_tag.hpp` | `types/types.hpp` lines 508-588 (`TypeTag` enum) | `hpactor` |
| `msg/message_id.hpp` | `types/types.hpp` line 245 + `adt/tags.hpp` line 20 | `hpactor` |
| `msg/typed_message.hpp` | `actor/typed_message.hpp` (whole file) | `hpactor` |
| `msg/request_timeout.hpp` | `types/request_timeout.hpp` (whole file) | `hpactor` |
| `msg/request_handle.hpp` | `types/request_handle.hpp` (whole file) | `hpactor` |
| `msg/delivery_mode.hpp` | `mailbox/delivery_mode.hpp` (whole file) | `hpactor::mailbox` |
| `msg/enqueue_result.hpp` | `mailbox/mailbox_policy.hpp` (partial: `EnqueueResultCode`, `EnqueueResult`, `failure_reason()`) | `hpactor::mailbox` |
| `msg/delivery_result.hpp` | `mailbox/delivery_result.hpp` (whole file) | `hpactor` + `hpactor::mailbox` |
| `msg/dedup_cache.hpp` | `mailbox/dedup_cache.hpp` (whole file) | `hpactor::mailbox` |
| `msg/dead_letter_record.hpp` | `mailbox/dead_letter_queue.hpp` (partial: `DeadLetterReason`, `DeadLetterSource`, mapping fns, `to_string()`, `DeadLetterRecord`) | `hpactor::mailbox` |
| `msg/frame.hpp` | `net/frame.hpp` (partial: `WireFrame` struct, flag constants) | `hpactor::net` |

### Files turned into shims

| Old path | Shim content |
|----------|-------------|
| `types/failure_reason.hpp` | `#include <hpactor/msg/failure_reason.hpp>` |
| `types/failure_envelope.hpp` | `#include <hpactor/msg/failure_envelope.hpp>` |
| `types/request_handle.hpp` | `#include <hpactor/msg/request_handle.hpp>` |
| `types/request_timeout.hpp` | `#include <hpactor/msg/request_timeout.hpp>` |
| `actor/typed_message.hpp` | `#include <hpactor/msg/typed_message.hpp>` |
| `mailbox/delivery_mode.hpp` | `#include <hpactor/msg/delivery_mode.hpp>` |
| `mailbox/delivery_result.hpp` | `#include <hpactor/msg/delivery_result.hpp>` |
| `mailbox/dedup_cache.hpp` | `#include <hpactor/msg/dedup_cache.hpp>` |

### Files modified (partial extraction)

| File | Change |
|------|--------|
| `types/types.hpp` | Extract `TypeTag` enum → `msg/type_tag.hpp`; include `msg/type_tag.hpp`; extract `MessageId` alias to `msg/message_id.hpp`; include `msg/message_id.hpp` |
| `adt/tags.hpp` | `MessageTag` extracted to `msg/message_id.hpp`; include shim |
| `mailbox/mailbox_policy.hpp` | Extract `EnqueueResultCode`/`EnqueueResult`/`failure_reason()` → `msg/enqueue_result.hpp`; include `msg/enqueue_result.hpp` |
| `mailbox/dead_letter_queue.hpp` | Extract `DeadLetterReason`/`DeadLetterSource`/`to_string()`/`DeadLetterRecord` → `msg/dead_letter_record.hpp`; include `msg/dead_letter_record.hpp` |
| `net/frame.hpp` | Extract `WireFrame` struct + flag constants → `msg/frame.hpp`; keep `to_proto`/`from_proto` address conversion fns; include `msg/frame.hpp` |

### `.cpp` files updated

| File | Change |
|------|--------|
| `src/types/failure_reason.cpp` | Update `#include` from `types/failure_reason.hpp` → `msg/failure_reason.hpp` |
| `src/net/frame.cpp` | Update `#include`; keep using `hpactor::net::WireFrame` |
| `src/mailbox/dedup_cache.cpp` | Update `#include` |

---

## Phase 1: Create `msg/` Subsystem + Compatibility Shims

### Task 1: Create `msg/fwd.hpp` — Forward Declarations Hub

**Files:**
- Create: `include/hpactor/msg/fwd.hpp`

- [ ] **Step 1: Write `msg/fwd.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#pragma once

#include <cstdint>
#include <memory>

namespace hpactor {

// Message identity
enum class TypeTag : uint32_t;
class TypedMessage;

// Delivery contracts
enum class FailureReason : uint8_t;
enum class FailureSource : uint8_t;
struct FailureEnvelope;
struct RequestTimeout;
template <typename T> class RequestHandle;

} // namespace hpactor

namespace hpactor::mailbox {

enum class DeliveryMode : uint8_t;
enum class DeliveryStatus : uint8_t;
struct DeliveryResult;
enum class EnqueueResultCode : uint8_t;
struct EnqueueResult;
class DedupCache;
enum class DeadLetterReason : uint8_t;
enum class DeadLetterSource : uint8_t;
struct DeadLetterRecord;

} // namespace hpactor::mailbox

namespace hpactor::net {

struct WireFrame;

} // namespace hpactor::net
```

- [ ] **Step 2: Verify file compiles in isolation**

Run: `echo '#include <hpactor/msg/fwd.hpp>' | g++ -std=c++20 -fsyntax-only -I include -x c++ -`

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/msg/fwd.hpp
git commit -m "feat(msg): add msg/fwd.hpp forward declarations hub

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 2: Move `FailureReason` + `FailureSource` to `msg/`

**Files:**
- Create: `include/hpactor/msg/failure_reason.hpp`
- Modify: `include/hpactor/types/failure_reason.hpp` → shim
- Modify: `src/types/failure_reason.cpp`

- [ ] **Step 1: Copy `types/failure_reason.hpp` to `msg/failure_reason.hpp` with updated includes**

Copy the entire content from `include/hpactor/types/failure_reason.hpp` to `include/hpactor/msg/failure_reason.hpp`. The file has no internal includes except `<cstdint>`, so no include path update needed.

- [ ] **Step 2: Replace old file with compatibility shim**

Write `include/hpactor/types/failure_reason.hpp`:
```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#pragma once
#include <hpactor/msg/failure_reason.hpp>
// Compatibility shim: FailureReason and FailureSource moved to msg/ subsystem.
// TODO: update consumers to include <hpactor/msg/failure_reason.hpp> directly.
```

- [ ] **Step 3: Update `src/types/failure_reason.cpp` include**

Change:
```cpp
#include <hpactor/types/failure_reason.hpp>
```
To:
```cpp
#include <hpactor/msg/failure_reason.hpp>
```

- [ ] **Step 4: Build and test**

Run:
```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
ctest --output-on-failure --parallel 8
```
Expected: All 32 GTest binaries pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/failure_reason.hpp include/hpactor/types/failure_reason.hpp src/types/failure_reason.cpp
git commit -m "feat(msg): move FailureReason and FailureSource to msg/ subsystem

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 3: Move `FailureEnvelope` to `msg/`

**Files:**
- Create: `include/hpactor/msg/failure_envelope.hpp`
- Modify: `include/hpactor/types/failure_envelope.hpp` → shim

- [ ] **Step 1: Copy and update includes**

Copy `types/failure_envelope.hpp` to `msg/failure_envelope.hpp`. Update its includes:
```cpp
// Before (in old location):
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

// After (in msg/):
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/types/types.hpp>
```
The `ref/actor_address.hpp` and `types/types.hpp` includes stay — these are leaf dependencies that `msg/` is allowed to depend on.

- [ ] **Step 2: Replace old file with shim**

```cpp
#pragma once
#include <hpactor/msg/failure_envelope.hpp>
// Compatibility shim: FailureEnvelope moved to msg/ subsystem.
```

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/msg/failure_envelope.hpp include/hpactor/types/failure_envelope.hpp
git commit -m "feat(msg): move FailureEnvelope to msg/ subsystem

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 4: Extract `TypeTag` to `msg/`

**Files:**
- Create: `include/hpactor/msg/type_tag.hpp`
- Modify: `include/hpactor/types/types.hpp` (partial extraction)

- [ ] **Step 1: Create `msg/type_tag.hpp`**

Extract lines 508-588 from `types/types.hpp` (the `TypeTag` enum + its doc comment) into `include/hpactor/msg/type_tag.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#pragma once

#include <cstdint>

namespace hpactor {

// TypeTag - type identifier for serialization (replaces RTTI)
//
// Range layout:
//   0x00000000 – 0x000000FF   System messages (256 slots)
//   0x00000100 – 0x00000FFF   Reserved for future system expansion
//   0x00001000 – 0x00FFFFFF   Application-defined messages (~16M slots)
//
// System sub-ranges:
//   0x00 – 0x0F   Core system (lifecycle: Down, Exit, Link, Unlink, Monitor, Demonitor)
//   0x10 – 0x1F   Spawn protocol (SpawnRequest, SpawnResponse, Error)
//   0x20 – 0x2F   HTTP protocol (HttpRequest, HttpResponse)
//   0x30 – 0x3F   TOML bootstrap (SystemInit)
//   0x40 – 0x4F   Metrics (MetricsRequest, MetricsResponse)
//   0x50 – 0x5F   CLI interactive (Inspect, Kill, List, Stats)
//   0x60 – 0x6F   Async I/O (IoCompletion)
//   0x70 – 0xFF   Reserved for future system use
//
// Application sub-ranges (examples):
//   0x00001000 – 0x00001FFF   Auth subsystem
//   0x00002000 – 0x00002FFF   Chat subsystem
//   0x00003000 – 0x00003FFF   Database subsystem
//   ...
// -----------------------------------------------------------------------------
enum class TypeTag : uint32_t {
    Invalid = 0x00000000,

    // Core system (0x00 – 0x0F)
    DownMsg = 0x01,
    ExitMsg = 0x02,
    LinkMsg = 0x03,
    UnlinkMsg = 0x04,
    MonitorMsg = 0x0A,
    DemonitorMsg = 0x0B,

    // Spawn protocol (0x10 – 0x1F)
    SpawnRequestTag = 0x10,
    SpawnResponseTag = 0x11,
    ErrorMsg = 0x12,

    // HTTP protocol (0x20 – 0x2F)
    HttpRequestTag = 0x20,
    HttpResponseTag = 0x21,

    // TOML config bootstrapping (0x30 – 0x3F)
    SystemInitTag = 0x30,

    // Metrics subsystem (0x40 – 0x4F)
    MetricsRequestTag = 0x40,
    MetricsResponseTag = 0x41,

    // CLI interactive subsystem (0x50 – 0x5F)
    InspectStateRequestTag = 0x50,
    InspectStateResponseTag = 0x51,
    KillRequestTag = 0x52,
    KillResponseTag = 0x53,
    ListActorsRequestTag = 0x54,
    ListActorsResponseTag = 0x55,
    SystemStatsRequestTag = 0x56,
    SystemStatsResponseTag = 0x57,
    MemoryStatsRequestTag = 0x58,
    MemoryStatsResponseTag = 0x59,
    TopologyShowRequestTag = 0x5A,
    TopologyShowResponseTag = 0x5B,
    TopologyRestartRequestTag = 0x5C,
    TopologyRestartResponseTag = 0x5D,
    QuarantineRequestTag = 0x5E,
    QuarantineResponseTag = 0x5F,

    // Async I/O (0x60 – 0x6F)
    IoCompletionTag = 0x60,

    // Backpressure control (0x70 – 0x7F)
    BackpressureSignalTag = 0x70,

    // ---- Application range ---------------------------------------------------
    User = 0x00001000,
};

} // namespace hpactor
```

- [ ] **Step 2: Modify `types/types.hpp` — replace TypeTag with include**

In `types/types.hpp`, replace lines 508-588 (the TypeTag comment block + enum definition) with:
```cpp
#include <hpactor/msg/type_tag.hpp>
```

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```
Expected: All 32 GTest binaries pass. `TypeTag` type is still available via `types/types.hpp` → `msg/type_tag.hpp`.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/msg/type_tag.hpp include/hpactor/types/types.hpp
git commit -m "feat(msg): extract TypeTag enum to msg/type_tag.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 5: Extract `MessageId` to `msg/`

**Files:**
- Create: `include/hpactor/msg/message_id.hpp`
- Modify: `include/hpactor/types/types.hpp` (extract `MessageId` alias)
- Modify: `include/hpactor/adt/tags.hpp` (extract `MessageTag`)

- [ ] **Step 1: Create `msg/message_id.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#pragma once

#include <hpactor/adt/id.hpp>

namespace hpactor {

/// Tag type for message identity.
struct MessageTag {};

/// Globally unique per-message identifier.
using MessageId = adt::Id<MessageTag>;

} // namespace hpactor
```

- [ ] **Step 2: Modify `adt/tags.hpp` — remove `MessageTag`**

Remove line 20: `struct MessageTag {};`. Add:
```cpp
#include <hpactor/msg/message_id.hpp>  // MessageTag, MessageId
```

- [ ] **Step 3: Modify `types/types.hpp` — remove direct `MessageId` alias**

Remove line 245: `using MessageId = Id<MessageTag>;`. The type is now available through `adt/tags.hpp` → `msg/message_id.hpp`.

- [ ] **Step 4: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/message_id.hpp include/hpactor/types/types.hpp include/hpactor/adt/tags.hpp
git commit -m "feat(msg): extract MessageId and MessageTag to msg/message_id.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 6: Move `TypedMessage` to `msg/`

**Files:**
- Create: `include/hpactor/msg/typed_message.hpp`
- Modify: `include/hpactor/actor/typed_message.hpp` → shim

- [ ] **Step 1: Copy and update includes**

Copy `actor/typed_message.hpp` to `msg/typed_message.hpp`. Update includes:
```cpp
// Before:
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/std_allocator.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

// After:
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/std_allocator.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>  // TraceContext, StreamBuffer, error, result
```
The `types/types.hpp` include is still needed for `TraceContext`, `StreamBuffer`, `INT64_MAX`, etc. The `TypeTag` and `MessageId` types now come transitively through `types/types.hpp`.

- [ ] **Step 2: Replace old file with shim**

```cpp
#pragma once
#include <hpactor/msg/typed_message.hpp>
// Compatibility shim: TypedMessage moved to msg/ subsystem.
```

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/msg/typed_message.hpp include/hpactor/actor/typed_message.hpp
git commit -m "feat(msg): move TypedMessage to msg/typed_message.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 7: Move `RequestTimeout` to `msg/`

**Files:**
- Create: `include/hpactor/msg/request_timeout.hpp`
- Modify: `include/hpactor/types/request_timeout.hpp` → shim

- [ ] **Step 1: Copy with updated includes**

Copy `types/request_timeout.hpp` to `msg/request_timeout.hpp`. No internal include changes needed (only `<chrono>` and `<cstdint>`).

- [ ] **Step 2: Replace old file with shim**

```cpp
#pragma once
#include <hpactor/msg/request_timeout.hpp>
// Compatibility shim: RequestTimeout moved to msg/ subsystem.
```

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/msg/request_timeout.hpp include/hpactor/types/request_timeout.hpp
git commit -m "feat(msg): move RequestTimeout to msg/request_timeout.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 8: Move `RequestHandle` to `msg/`

**Files:**
- Create: `include/hpactor/msg/request_handle.hpp`
- Modify: `include/hpactor/types/request_handle.hpp` → shim

- [ ] **Step 1: Copy with updated includes**

Copy `types/request_handle.hpp` to `msg/request_handle.hpp`. Update includes — `types/types.hpp` is needed for `result<T>`, `error`, `errors::cancelled`, `MessageId`. No path changes needed since we're keeping the `types/types.hpp` include.

- [ ] **Step 2: Replace old file with shim**

```cpp
#pragma once
#include <hpactor/msg/request_handle.hpp>
// Compatibility shim: RequestHandle moved to msg/ subsystem.
```

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/msg/request_handle.hpp include/hpactor/types/request_handle.hpp
git commit -m "feat(msg): move RequestHandle to msg/request_handle.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 9: Move `DeliveryMode` to `msg/`

**Files:**
- Create: `include/hpactor/msg/delivery_mode.hpp`
- Modify: `include/hpactor/mailbox/delivery_mode.hpp` → shim

- [ ] **Step 1: Copy with updated includes**

Copy `mailbox/delivery_mode.hpp` to `msg/delivery_mode.hpp`. It only includes `<cstdint>`. The `namespace hpactor::mailbox` stays — we only move the physical file, not the namespace.

- [ ] **Step 2: Replace old file with shim**

```cpp
#pragma once
#include <hpactor/msg/delivery_mode.hpp>
// Compatibility shim: DeliveryMode moved to msg/ subsystem.
```

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/msg/delivery_mode.hpp include/hpactor/mailbox/delivery_mode.hpp
git commit -m "feat(msg): move DeliveryMode to msg/delivery_mode.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 10: Extract `EnqueueResultCode` + `EnqueueResult` to `msg/`

**Files:**
- Create: `include/hpactor/msg/enqueue_result.hpp`
- Modify: `include/hpactor/mailbox/mailbox_policy.hpp` (partial extraction)

- [ ] **Step 1: Create `msg/enqueue_result.hpp`**

Extract from `mailbox/mailbox_policy.hpp`:
- `EnqueueResultCode` enum
- `failure_reason(EnqueueResultCode)` constexpr function
- `EnqueueResult` struct with all its methods

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#pragma once

#include <hpactor/actor/typed_message.hpp>  // TypeTag (via shim → msg/)
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/msg/delivery_mode.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>

namespace hpactor::mailbox {

// ... copy the BackpressureReason enum if EnqueueResult uses it ...

enum class EnqueueResultCode : uint8_t {
    Accepted,
    AcceptedWithSoftPressure,
    Rejected,
    DroppedNewest,
    DroppedExisting,
    ReroutedToDeadLetter,
    ReroutedToOverflow,
    MailboxClosed,
    ActorNotFound,
    EndpointBackpressure,
    EndpointCircuitOpen,
    CircuitOpen,
};

[[nodiscard]] constexpr FailureReason
failure_reason(EnqueueResultCode code) noexcept {
    // ... exact copy from mailbox_policy.hpp lines 133-160 ...
}

struct EnqueueResult {
    EnqueueResultCode code = EnqueueResultCode::Accepted;
    ActorId target;
    uint32_t depth = 0;
    uint32_t capacity = 0;
    uint64_t bytes = 0;
    uint64_t byte_capacity = 0;
    BackpressureReason pressure_reason = BackpressureReason::HighWatermark;
    MailboxPressureState pressure_state = MailboxPressureState::Normal;
    // ... remaining fields and methods ...

    [[nodiscard]] bool accepted() const noexcept {
        return code == EnqueueResultCode::Accepted ||
               code == EnqueueResultCode::AcceptedWithSoftPressure;
    }
    [[nodiscard]] FailureReason to_failure_reason() const noexcept {
        return failure_reason(code);
    }
};

} // namespace hpactor::mailbox
```

Note: `EnqueueResult` uses `BackpressureReason`, `MailboxPressureState`, and other types defined in `mailbox/mailbox_policy.hpp`. These types are mailbox internals and **stay** in `mailbox/mailbox_policy.hpp`. The `msg/enqueue_result.hpp` file must include the mailbox header for these types.

Wait — this creates a dependency FROM `msg/` TO `mailbox/`, which violates our dependency rule. Let me reconsider the extraction approach.

**Revised approach:** Instead of extracting `EnqueueResultCode` and `EnqueueResult` to standalone `msg/` files, keep the canonical definitions in `mailbox/mailbox_policy.hpp` and create an **alias/shim header** at `msg/enqueue_result.hpp` that just includes the mailbox header. This means `EnqueueResultCode` and `EnqueueResult` physically stay in `mailbox/` but a `msg/` header is available for consumers that want to think in `msg/` terms.

Actually, the simpler approach: keep `EnqueueResultCode`, `EnqueueResult`, and their helpers in `mailbox/mailbox_policy.hpp` but add `#include <hpactor/msg/enqueue_result.hpp>` at the top of that file. The canonical definition lives in `msg/enqueue_result.hpp`, which includes `mailbox/mailbox_policy.hpp` for the types it needs. Actually, that creates circular includes.

Let me think about this differently. The `EnqueueResult` struct has mailbox-specific fields (`depth`, `capacity`, `byte_capacity`, `pressure_reason`, `pressure_state`). It IS a mailbox concept. The design says it moves to `msg/`, but it has a hard dependency on mailbox types (`BackpressureReason`, `MailboxPressureState`).

**Best approach:** Move `BackpressureReason`, `MailboxPressureState`, `EnqueueResultCode`, and `EnqueueResult` into a single `msg/enqueue_result.hpp` file. `BackpressureReason` and `MailboxPressureState` are enums with no include dependencies (just `<cstdint>`). This keeps the `msg/` dependency away from `mailbox/`.

Let me check if `BackpressureReason` and `MailboxPressureState` have any non-`<cstdint>` dependencies...

Looking at mailbox_policy.hpp:
- `BackpressureReason` is an enum class (lines 101-107) — only `<cstdint>` needed
- `MailboxPressureState` is an enum class (lines 56-61) — only `<cstdint>` needed

Both can safely move to `msg/enqueue_result.hpp` without creating a mailbox dependency.

- [ ] **Step 1: Create `msg/enqueue_result.hpp` with all needed types**

Copy from `mailbox/mailbox_policy.hpp` lines 56-61 (`MailboxPressureState`), lines 101-107 (`BackpressureReason`), lines 109-215 (`EnqueueResultCode`, `failure_reason()`, `EnqueueResult` struct with all methods including `to_delivery_result()` declaration). Also copy the `DeliveryResult` forward declaration from lines 28-31.

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#pragma once

#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>

namespace hpactor::mailbox {

// Forward declaration — defined in delivery_result.hpp (also moving to msg/).
struct DeliveryResult;

enum class MailboxPressureState : uint8_t {
    Normal, SoftPressure, HardPressure, Recovering,
};

enum class BackpressureReason : uint8_t {
    HighWatermark, HardCapacity, ByteCapacity,
    OverflowPolicy, NodeMemoryPressure,
};

enum class EnqueueResultCode : uint8_t {
    Accepted, AcceptedWithSoftPressure, Rejected,
    DroppedNewest, DroppedExisting, ReroutedToDeadLetter,
    ReroutedToOverflow, MailboxClosed, ActorNotFound,
    EndpointBackpressure, EndpointCircuitOpen, CircuitOpen,
};

/// Map EnqueueResultCode to FailureReason.
[[nodiscard]] constexpr FailureReason
failure_reason(EnqueueResultCode code) noexcept {
    switch (code) {
        case EnqueueResultCode::Rejected: return FailureReason::RejectedByPolicy;
        case EnqueueResultCode::DroppedNewest:
        case EnqueueResultCode::DroppedExisting: return FailureReason::Dropped;
        case EnqueueResultCode::ReroutedToDeadLetter: return FailureReason::RejectedByPolicy;
        case EnqueueResultCode::ReroutedToOverflow: return FailureReason::RejectedByPolicy;
        case EnqueueResultCode::MailboxClosed: return FailureReason::MailboxClosed;
        case EnqueueResultCode::ActorNotFound: return FailureReason::NoRoute;
        case EnqueueResultCode::EndpointBackpressure: return FailureReason::ResourceExhausted;
        case EnqueueResultCode::EndpointCircuitOpen: return FailureReason::RemoteUnavailable;
        case EnqueueResultCode::CircuitOpen: return FailureReason::CircuitOpen;
        case EnqueueResultCode::Accepted:
        case EnqueueResultCode::AcceptedWithSoftPressure: return FailureReason::Unknown;
    }
    return FailureReason::Unknown;
}

struct EnqueueResult {
    EnqueueResultCode code = EnqueueResultCode::Accepted;
    ActorId target;
    uint32_t depth = 0;
    uint32_t capacity = 0;
    uint64_t bytes = 0;
    uint64_t byte_capacity = 0;
    BackpressureReason pressure_reason = BackpressureReason::HighWatermark;
    MailboxPressureState pressure_state = MailboxPressureState::Normal;
    double pressure_ratio = 0.0;
    std::chrono::milliseconds retry_after{0};
    TypeTag affected_type = TypeTag::Invalid;
    uint64_t affected_message_id = 0;

    [[nodiscard]] EnqueueResultCode status() const noexcept { return code; }
    [[nodiscard]] bool ok() const noexcept { return accepted(); }
    [[nodiscard]] bool accepted() const noexcept {
        return code == EnqueueResultCode::Accepted ||
               code == EnqueueResultCode::AcceptedWithSoftPressure ||
               code == EnqueueResultCode::ReroutedToOverflow;
    }
    [[nodiscard]] bool retryable() const noexcept {
        return code == EnqueueResultCode::Rejected ||
               code == EnqueueResultCode::MailboxClosed ||
               code == EnqueueResultCode::ReroutedToOverflow ||
               code == EnqueueResultCode::EndpointBackpressure ||
               code == EnqueueResultCode::CircuitOpen;
    }
    [[nodiscard]] FailureReason failure_reason() const noexcept {
        return mailbox::failure_reason(code);
    }
    /// Convert to user-facing DeliveryResult. Requires delivery_result.hpp at
    /// the call site (return type is complete only after inclusion).
    DeliveryResult to_delivery_result(const ActorAddress&, MessageId = {}) const;
};

} // namespace hpactor::mailbox
```

- [ ] **Step 2: Modify `mailbox/mailbox_policy.hpp`**

Remove lines 56-61 (`MailboxPressureState`), lines 101-107 (`BackpressureReason`), and lines 109-215 (`EnqueueResultCode`, `failure_reason()`, `EnqueueResult`). Also remove the `DeliveryResult` forward declaration on lines 28-31 (it moves to `msg/enqueue_result.hpp`).

Add at top:
```cpp
#include <hpactor/msg/enqueue_result.hpp>
```

Keep: `MailboxCapacity`, `OverflowPolicy`, `BackpressureMode`, `MailboxConfig`, `MessagePriority`, `DeliveryOptions`, `MailboxEnvelopeMeta`, `BackpressureSignal`, `is_system_message()`, `estimate_message_bytes()`, `MailboxPolicy` class.

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/msg/enqueue_result.hpp include/hpactor/mailbox/mailbox_policy.hpp
git commit -m "feat(msg): extract EnqueueResult and admission types to msg/enqueue_result.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 11: Move `DeliveryStatus` + `DeliveryResult` to `msg/`

**Files:**
- Create: `include/hpactor/msg/delivery_result.hpp`
- Modify: `include/hpactor/mailbox/delivery_result.hpp` → shim

- [ ] **Step 1: Copy with updated includes**

Copy `mailbox/delivery_result.hpp` to `msg/delivery_result.hpp`. Update its includes:
```cpp
// Before:
#include <hpactor/mailbox/mailbox_policy.hpp>  // for DeliveryMode, BackpressureReason + circular include guard
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

// After:
#include <hpactor/msg/delivery_mode.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>
```

Check if the `mailbox/mailbox_policy.hpp` include was providing anything beyond `DeliveryMode` that is now needed. `TransportSendResult` (lines 32-53) and `DeliveryStatus`/`DeliveryResult` don't depend on `EnqueueResult` types.

- [ ] **Step 2: Replace old file with shim**

```cpp
#pragma once
#include <hpactor/msg/delivery_result.hpp>
// Compatibility shim: DeliveryStatus, DeliveryResult, TransportSendResult moved to msg/ subsystem.
```

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/msg/delivery_result.hpp include/hpactor/mailbox/delivery_result.hpp
git commit -m "feat(msg): move DeliveryStatus, DeliveryResult, TransportSendResult to msg/delivery_result.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 12: Move `DedupCache` to `msg/`

**Files:**
- Create: `include/hpactor/msg/dedup_cache.hpp`
- Modify: `include/hpactor/mailbox/dedup_cache.hpp` → shim
- Modify: `src/mailbox/dedup_cache.cpp`

- [ ] **Step 1: Copy with updated includes**

Copy `mailbox/dedup_cache.hpp` to `msg/dedup_cache.hpp`. Update includes:
```cpp
// Before:
#include <hpactor/types/types.hpp>

// After:
#include <hpactor/types/types.hpp>  // EndPoint, ActorId, MessageId
```
The `types/types.hpp` include stays — needed for `EndPoint`, `ActorId`, `MessageId`.

- [ ] **Step 2: Replace old file with shim**

```cpp
#pragma once
#include <hpactor/msg/dedup_cache.hpp>
// Compatibility shim: DedupCache moved to msg/ subsystem.
```

- [ ] **Step 3: Update `src/mailbox/dedup_cache.cpp` include**

Change `#include <hpactor/mailbox/dedup_cache.hpp>` → `#include <hpactor/msg/dedup_cache.hpp>`.

- [ ] **Step 4: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/dedup_cache.hpp include/hpactor/mailbox/dedup_cache.hpp src/mailbox/dedup_cache.cpp
git commit -m "feat(msg): move DedupCache to msg/dedup_cache.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 13: Extract `DeadLetterRecord` + Related Types to `msg/`

**Files:**
- Create: `include/hpactor/msg/dead_letter_record.hpp`
- Modify: `include/hpactor/mailbox/dead_letter_queue.hpp` (partial extraction)

- [ ] **Step 1: Create `msg/dead_letter_record.hpp`**

Copy exactly from `mailbox/dead_letter_queue.hpp` lines 30-236: `DeadLetterReason` enum, `DeadLetterSource` enum, `failure_reason(DeadLetterReason)`, `failure_source(DeadLetterSource)`, `to_string(DeadLetterReason)`, `to_string(DeadLetterSource)`, and `DeadLetterRecord` struct with `to_failure_envelope()` method.

Update includes — all internal references now point within `msg/`:
```cpp
#include <hpactor/adt/stream_buffer.hpp>     // StreamBuffer (payload_sample)
#include <hpactor/msg/failure_envelope.hpp>   // FailureEnvelope
#include <hpactor/msg/failure_reason.hpp>     // FailureReason, FailureSource
#include <hpactor/ref/actor_address.hpp>      // ActorAddress
#include <hpactor/types/types.hpp>            // TypeTag, MessageId, TraceContext
```

Content is a verbatim copy — all enums, functions, and the `DeadLetterRecord` struct with its `to_failure_envelope()` method. No changes to namespace or type definitions.

- [ ] **Step 2: Modify `mailbox/dead_letter_queue.hpp`**

Remove `DeadLetterReason`, `DeadLetterSource`, both `failure_reason`/`failure_source` mapping functions, both `to_string()` functions, and `DeadLetterRecord` struct (lines 30-236). Add at top:
```cpp
#include <hpactor/msg/dead_letter_record.hpp>
```

Keep: `DeadLetterOverflowPolicy`, `DeadLetterConfig`, `DeadLetterQueueSnapshot`, `IDeadLetterSink`, `DeadLetterQueue` class and all its methods.

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/msg/dead_letter_record.hpp include/hpactor/mailbox/dead_letter_queue.hpp
git commit -m "feat(msg): extract DeadLetterRecord and related types to msg/dead_letter_record.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 14: Move `WireFrame` to `msg/`

**Files:**
- Create: `include/hpactor/msg/frame.hpp`
- Modify: `include/hpactor/net/frame.hpp` (partial extraction)
- Modify: `src/net/frame.cpp`

- [ ] **Step 1: Create `msg/frame.hpp` with WireFrame struct**

Extract from `net/frame.hpp` lines 28-69 — the `WireFrame` struct and flag constants. **Drop** the `#include <hpactor/actor/abstract_actor.hpp>` — it's not needed by `WireFrame`.

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#pragma once

#include <hpactor/frame.pb.h>
#include <hpactor/types/types.hpp>
#include <span>

namespace hpactor::net {

struct WireFrame {
    static constexpr uint32_t MagicHeader = 0x43415048; // "HPAC" (little-endian)
    static constexpr size_t HeaderSize = 8;

    uint32_t magic_hdr = MagicHeader;
    size_t length;
    ::hpactor::net::ActorMsgFrame pb_frame;

    StreamBuffer encode() const;
    static WireFrame decode(const StreamBuffer& data);
    static WireFrame decode(std::span<const uint8_t> data);

    // Flag constants
    static constexpr uint32_t Important = 1 << 0;
    static constexpr uint32_t NoDrop = 1 << 1;
    static constexpr uint32_t RpcRequest = 1 << 2;
    static constexpr uint32_t RpcResponse = 1 << 3;
    static constexpr uint32_t RpcIdempotent = 1 << 4;
};

} // namespace hpactor::net
```

- [ ] **Step 2: Modify `net/frame.hpp` — keep address conversion, add msg include**

Remove `WireFrame` struct and flag constants. Add:
```cpp
#include <hpactor/msg/frame.hpp>
```

Keep: `to_proto()` / `from_proto()` address conversion functions, `PbTraceContext` conversion. These stay in `net/` because they belong to the network address layer, not message framing.

Also remove the `#include <hpactor/actor/abstract_actor.hpp>` if nothing else in the file uses it.

- [ ] **Step 3: Update `src/net/frame.cpp` includes**

```cpp
// Add:
#include <hpactor/msg/frame.hpp>
// Keep existing includes for to_proto/from_proto implementation
```

- [ ] **Step 4: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/frame.hpp include/hpactor/net/frame.hpp src/net/frame.cpp
git commit -m "feat(msg): move WireFrame to msg/frame.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 15: Phase 1 Final Verification

- [ ] **Step 1: Full clean build**

```bash
rm -rf build
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
```
Expected: Build succeeds with zero errors.

- [ ] **Step 2: Full test suite**

```bash
ctest --output-on-failure --parallel 8
```
Expected: All 32 GTest binaries pass (1411+ tests).

- [ ] **Step 3: Sanitizer build**

```bash
rm -rf build
cmake -S . -B build -GNinja -DENABLE_ASAN=ON
ninja -C build
ctest --output-on-failure --parallel 8
```
Expected: All tests pass.

- [ ] **Step 4: Verify include hygiene**

Run this script to verify each `msg/` header is self-contained:
```bash
for f in include/hpactor/msg/*.hpp; do
    echo "Checking: $f"
    echo "#include <${f#include/}>" | g++ -std=c++20 -fsyntax-only -I include -x c++ - || exit 1
done
echo "All msg/ headers are self-contained"
```

- [ ] **Step 5: Verify `msg/` does not include forbidden headers**

```bash
! grep -r 'hpactor/actor/\|hpactor/mailbox/\|hpactor/core/\|hpactor/sched/\|hpactor/rpc/' include/hpactor/msg/
echo "msg/ dependency check passed"
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(msg): complete Phase 1 — all msg/ headers created with shims

All 32 GTest binaries pass. msg/ subsystem is self-contained.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 2: Update Internal Consumers

For each subsystem, update includes to use `msg/` paths directly instead of relying on shims. Build and test after each subsystem.

### Task 16: Update `src/` consumer includes

- [ ] **Step 1: Find all files using old paths**

```bash
grep -rn "types/failure_reason.hpp\|types/failure_envelope.hpp\|types/request_handle.hpp\|types/request_timeout.hpp\|actor/typed_message.hpp\|mailbox/delivery_mode.hpp\|mailbox/delivery_result.hpp\|mailbox/dedup_cache.hpp" src/ --include='*.cpp' --include='*.hpp' -l | sort
```

- [ ] **Step 2: Update each file**

For each file found, replace old `#include` paths with `msg/` paths. Use `sed` for bulk replacement:

```bash
# Update types/ shim paths to msg/ canonical paths
find src/ -name '*.cpp' -o -name '*.hpp' | xargs sed -i '' \
    -e 's|#include <hpactor/types/failure_reason.hpp>|#include <hpactor/msg/failure_reason.hpp>|g' \
    -e 's|#include <hpactor/types/failure_envelope.hpp>|#include <hpactor/msg/failure_envelope.hpp>|g' \
    -e 's|#include <hpactor/types/request_handle.hpp>|#include <hpactor/msg/request_handle.hpp>|g' \
    -e 's|#include <hpactor/types/request_timeout.hpp>|#include <hpactor/msg/request_timeout.hpp>|g' \
    -e 's|#include <hpactor/actor/typed_message.hpp>|#include <hpactor/msg/typed_message.hpp>|g' \
    -e 's|#include <hpactor/mailbox/delivery_mode.hpp>|#include <hpactor/msg/delivery_mode.hpp>|g' \
    -e 's|#include <hpactor/mailbox/delivery_result.hpp>|#include <hpactor/msg/delivery_result.hpp>|g' \
    -e 's|#include <hpactor/mailbox/dedup_cache.hpp>|#include <hpactor/msg/dedup_cache.hpp>|g'
```

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 4: Commit**

```bash
git add src/
git commit -m "refactor(msg): update src/ includes to msg/ canonical paths

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 17: Update `tests/` consumer includes

- [ ] **Step 1: Find and update test files**

```bash
find tests/ -name '*.cpp' -o -name '*.hpp' | xargs sed -i '' \
    -e 's|#include <hpactor/types/failure_reason.hpp>|#include <hpactor/msg/failure_reason.hpp>|g' \
    -e 's|#include <hpactor/types/failure_envelope.hpp>|#include <hpactor/msg/failure_envelope.hpp>|g' \
    -e 's|#include <hpactor/types/request_handle.hpp>|#include <hpactor/msg/request_handle.hpp>|g' \
    -e 's|#include <hpactor/types/request_timeout.hpp>|#include <hpactor/msg/request_timeout.hpp>|g' \
    -e 's|#include <hpactor/actor/typed_message.hpp>|#include <hpactor/msg/typed_message.hpp>|g' \
    -e 's|#include <hpactor/mailbox/delivery_mode.hpp>|#include <hpactor/msg/delivery_mode.hpp>|g' \
    -e 's|#include <hpactor/mailbox/delivery_result.hpp>|#include <hpactor/msg/delivery_result.hpp>|g' \
    -e 's|#include <hpactor/mailbox/dedup_cache.hpp>|#include <hpactor/msg/dedup_cache.hpp>|g'
```

- [ ] **Step 2: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 3: Commit**

```bash
git add tests/
git commit -m "refactor(msg): update tests/ includes to msg/ canonical paths

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 18: Update `apps/` and `examples/` includes

- [ ] **Step 1: Find and update app/example files**

```bash
find apps/ examples/ -name '*.cpp' -o -name '*.hpp' | xargs sed -i '' \
    -e 's|#include <hpactor/types/failure_reason.hpp>|#include <hpactor/msg/failure_reason.hpp>|g' \
    -e 's|#include <hpactor/types/failure_envelope.hpp>|#include <hpactor/msg/failure_envelope.hpp>|g' \
    -e 's|#include <hpactor/types/request_handle.hpp>|#include <hpactor/msg/request_handle.hpp>|g' \
    -e 's|#include <hpactor/types/request_timeout.hpp>|#include <hpactor/msg/request_timeout.hpp>|g' \
    -e 's|#include <hpactor/actor/typed_message.hpp>|#include <hpactor/msg/typed_message.hpp>|g' \
    -e 's|#include <hpactor/mailbox/delivery_mode.hpp>|#include <hpactor/msg/delivery_mode.hpp>|g' \
    -e 's|#include <hpactor/mailbox/delivery_result.hpp>|#include <hpactor/msg/delivery_result.hpp>|g' \
    -e 's|#include <hpactor/mailbox/dedup_cache.hpp>|#include <hpactor/msg/dedup_cache.hpp>|g'
```

- [ ] **Step 2: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 3: Commit**

```bash
git add apps/ examples/
git commit -m "refactor(msg): update apps/ and examples/ includes to msg/ canonical paths

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 19: Update `tools/` includes

- [ ] **Step 1: Find and update tool files**

```bash
find tools/ -name '*.cpp' -o -name '*.hpp' | xargs sed -i '' \
    -e 's|#include <hpactor/types/failure_reason.hpp>|#include <hpactor/msg/failure_reason.hpp>|g' \
    -e 's|#include <hpactor/types/failure_envelope.hpp>|#include <hpactor/msg/failure_envelope.hpp>|g' \
    -e 's|#include <hpactor/types/request_handle.hpp>|#include <hpactor/msg/request_handle.hpp>|g' \
    -e 's|#include <hpactor/types/request_timeout.hpp>|#include <hpactor/msg/request_timeout.hpp>|g' \
    -e 's|#include <hpactor/actor/typed_message.hpp>|#include <hpactor/msg/typed_message.hpp>|g' \
    -e 's|#include <hpactor/mailbox/delivery_mode.hpp>|#include <hpactor/msg/delivery_mode.hpp>|g' \
    -e 's|#include <hpactor/mailbox/delivery_result.hpp>|#include <hpactor/msg/delivery_result.hpp>|g' \
    -e 's|#include <hpactor/mailbox/dedup_cache.hpp>|#include <hpactor/msg/dedup_cache.hpp>|g'
```

- [ ] **Step 2: Build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```

- [ ] **Step 3: Commit**

```bash
git add tools/
git commit -m "refactor(msg): update tools/ includes to msg/ canonical paths

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 20: Phase 2 Final Verification

- [ ] **Step 1: Verify zero references to old shim paths**

```bash
# Should produce no output (all consumers updated)
grep -rn "types/failure_reason.hpp\|types/failure_envelope.hpp\|types/request_handle.hpp\|types/request_timeout.hpp\|actor/typed_message.hpp\|mailbox/delivery_mode.hpp\|mailbox/delivery_result.hpp\|mailbox/dedup_cache.hpp" src/ tests/ apps/ examples/ tools/ --include='*.cpp' --include='*.hpp'
```
Expected: No output (all includes updated to `msg/` paths).

- [ ] **Step 2: Full build and test**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```
Expected: All tests pass.

- [ ] **Step 3: Commit**

```bash
git commit -m "refactor(msg): Phase 2 complete — all consumers use msg/ canonical paths

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 3: Remove Compatibility Shims

### Task 21: Remove old shim headers

- [ ] **Step 1: Delete each shim file and verify**

Remove the 8 shim headers that are now pure includes:
```bash
rm include/hpactor/types/failure_reason.hpp
rm include/hpactor/types/failure_envelope.hpp
rm include/hpactor/types/request_handle.hpp
rm include/hpactor/types/request_timeout.hpp
rm include/hpactor/actor/typed_message.hpp
rm include/hpactor/mailbox/delivery_mode.hpp
rm include/hpactor/mailbox/delivery_result.hpp
rm include/hpactor/mailbox/dedup_cache.hpp
```

- [ ] **Step 2: Full clean build**

```bash
ninja -C build
```
Expected: Build succeeds. If any consumer still references old paths, the build fails — fix those files by updating their includes to `msg/` paths.

- [ ] **Step 3: Full test suite**

```bash
ctest --output-on-failure --parallel 8
```
Expected: All 32 GTest binaries pass.

- [ ] **Step 4: Final sanitizer verification**

```bash
rm -rf build
cmake -S . -B build -GNinja -DENABLE_ASAN=ON
ninja -C build && ctest --output-on-failure --parallel 8
```
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor(msg): Phase 3 complete — remove compatibility shims

All consumers now use msg/ canonical paths directly.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Post-Implementation Checklist

- [ ] `include/hpactor/msg/` contains all 14 headers
- [ ] All 8 shim headers removed
- [ ] `msg/` has zero includes of `actor/`, `mailbox/`, `core/`, `net/`, `sched/`, `rpc/`
- [ ] All 32 GTest binaries pass (ASAN and normal)
- [ ] Full `ninja -C build` succeeds with zero warnings
- [ ] Design spec (`docs/superpowers/specs/2026-06-06-msg-subsystem-extraction-design.md`) is satisfied
