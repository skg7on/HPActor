# Structured Failure Envelope — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `FailureReason`, `FailureSource`, and `FailureEnvelope` types; map existing `EnqueueResultCode` and `errors::` codes onto the shared vocabulary; wire `try_deliver_local()` to build a full `FailureEnvelope` on every failure path; emit a `kDeliveryFailure` metric event.

**Architecture:** Two new header-only type files (`failure_reason.hpp`, `failure_envelope.hpp`) with a compiled `to_string()` translation unit. Existing structs (`EnqueueResult`, `error`) gain a mapping method that returns `FailureReason` without taking a dependency on the heavier `FailureEnvelope`. The full envelope is built at the delivery boundary (`actor_system.cpp`) where sender, receiver, message_id, and trace context are available.

**Tech Stack:** C++20, no-exceptions, no-RTTI, LLVM style, header-only types + compiled hpactor_lib.

**Files map:**

| File | Role |
|------|------|
| `include/hpactor/types/failure_reason.hpp` (new) | `FailureReason` enum, `FailureSource` enum, `retryable()`, `to_string()` decl |
| `include/hpactor/types/failure_envelope.hpp` (new) | `FailureEnvelope` struct, `make_failure_envelope()` factory |
| `src/types/failure_reason.cpp` (new) | `to_string()` tables |
| `include/hpactor/types/types.hpp` (modify) | `error` gains `failure_reason()` method |
| `include/hpactor/mailbox/mailbox_policy.hpp` (modify) | `failure_reason(EnqueueResultCode)` free function, `EnqueueResult::failure_reason()` |
| `include/hpactor/metrics/metrics_event.hpp` (modify) | Add `kDeliveryFailure = 20` to `MetricEventType` |
| `src/actor/actor_system.cpp` (modify) | Build `FailureEnvelope` on each failure path in `try_deliver_local()` |
| `tests/core/test_failure_reason.cpp` (new) | Unit tests for enum values, mapping, retryable, to_string |
| `tests/core/test_failure_envelope.cpp` (new) | Unit tests for envelope construction and factory |
| `tests/CMakeLists.txt` (modify) | Register the two new test executables |
| `src/CMakeLists.txt` (modify) | Add `src/types/failure_reason.cpp` to hpactor_lib |

---

### Task 1: Create `FailureReason` and `FailureSource` enums

**Files:**
- Create: `include/hpactor/types/failure_reason.hpp`

- [ ] **Step 1: Write the header**

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

#include <cstdint>

namespace hpactor {

/// Canonical failure reason shared across actor send, ask, RPC, spawn,
/// DLQ, and tracing. Every failed delivery or runtime control failure
/// maps to one of these codes.
enum class FailureReason : uint8_t {
    // ── Route / addressing (0-9) ────────────────────────────────
    NoRoute = 0,
    NodeUnavailable = 1,

    // ── Actor lifecycle (10-19) ─────────────────────────────────
    ActorDead = 10,
    ActorNotReady = 11,
    Quarantined = 12,
    CircuitOpen = 13,

    // ── Resource limits (20-29) ─────────────────────────────────
    MailboxFull = 20,
    OutboundQueueFull = 21,
    MemoryPressure = 22,

    // ── Time (30-39) ────────────────────────────────────────────
    Expired = 30,
    Timeout = 31,

    // ── Policy (40-49) ──────────────────────────────────────────
    RejectedByPolicy = 40,
    Dropped = 41,
    MailboxClosed = 42,

    // ── Transport / serialization (50-59) ───────────────────────
    SerializationError = 50,
    TransportError = 51,
    FrameRejected = 52,

    // ── Deduplication (60-69) ───────────────────────────────────
    Duplicate = 60,

    // ── Graceful shutdown (70-79) ───────────────────────────────
    Draining = 70,
    ShuttingDown = 71,

    // ── Reliable messaging (80-89) ──────────────────────────────
    RetryExhausted = 80,

    // ── Spawn (90-99) ───────────────────────────────────────────
    SpawnFailed = 90,

    // ── Sentinel ────────────────────────────────────────────────
    Unknown = 255,
};

/// Which subsystem produced a failure. Combined with FailureReason to
/// disambiguate context (e.g. Timeout from ActorRuntime vs Rpc).
enum class FailureSource : uint8_t {
    ActorRuntime,
    Mailbox,
    Rpc,
    Transport,
    Discovery,
    Scheduler,
    Config,
    Security,
    DurableStore,
    Supervision,
    Cluster,
    Unknown,
};

/// Whether the caller can retry with a reasonable chance of success.
constexpr bool retryable(FailureReason reason) noexcept {
    switch (reason) {
        case FailureReason::NoRoute:
        case FailureReason::NodeUnavailable:
        case FailureReason::ActorNotReady:
        case FailureReason::CircuitOpen:
        case FailureReason::MailboxFull:
        case FailureReason::OutboundQueueFull:
        case FailureReason::MemoryPressure:
        case FailureReason::Timeout:
        case FailureReason::TransportError:
        case FailureReason::Draining:
        case FailureReason::ShuttingDown:
            return true;
        default:
            return false;
    }
}

/// Human-readable snake_case string for metrics labels, log keys, and CLI.
const char* to_string(FailureReason reason) noexcept;

/// Human-readable snake_case string for the subsystem source.
const char* to_string(FailureSource source) noexcept;

} // namespace hpactor
```

- [ ] **Step 2: Verify the header compiles in isolation**

```bash
cd .worktrees/failure-envelope-spec && echo '#include <hpactor/types/failure_reason.hpp>' | g++ -std=c++20 -fsyntax-only -I include -x c++ -
```
Expected: No output (compiles clean).

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/types/failure_reason.hpp
git commit -m "feat(types): add FailureReason and FailureSource shared enums"
```

---

### Task 2: Create `to_string()` translation unit

**Files:**
- Create: `src/types/failure_reason.cpp`

- [ ] **Step 1: Write the .cpp**

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

#include <hpactor/types/failure_reason.hpp>

namespace hpactor {

const char* to_string(FailureReason reason) noexcept {
    switch (reason) {
        case FailureReason::NoRoute:
            return "no_route";
        case FailureReason::NodeUnavailable:
            return "node_unavailable";
        case FailureReason::ActorDead:
            return "actor_dead";
        case FailureReason::ActorNotReady:
            return "actor_not_ready";
        case FailureReason::Quarantined:
            return "quarantined";
        case FailureReason::CircuitOpen:
            return "circuit_open";
        case FailureReason::MailboxFull:
            return "mailbox_full";
        case FailureReason::OutboundQueueFull:
            return "outbound_queue_full";
        case FailureReason::MemoryPressure:
            return "memory_pressure";
        case FailureReason::Expired:
            return "expired";
        case FailureReason::Timeout:
            return "timeout";
        case FailureReason::RejectedByPolicy:
            return "rejected_by_policy";
        case FailureReason::Dropped:
            return "dropped";
        case FailureReason::MailboxClosed:
            return "mailbox_closed";
        case FailureReason::SerializationError:
            return "serialization_error";
        case FailureReason::TransportError:
            return "transport_error";
        case FailureReason::FrameRejected:
            return "frame_rejected";
        case FailureReason::Duplicate:
            return "duplicate";
        case FailureReason::Draining:
            return "draining";
        case FailureReason::ShuttingDown:
            return "shutting_down";
        case FailureReason::RetryExhausted:
            return "retry_exhausted";
        case FailureReason::SpawnFailed:
            return "spawn_failed";
        case FailureReason::Unknown:
            return "unknown";
    }
    return "unknown";
}

const char* to_string(FailureSource source) noexcept {
    switch (source) {
        case FailureSource::ActorRuntime:
            return "actor_runtime";
        case FailureSource::Mailbox:
            return "mailbox";
        case FailureSource::Rpc:
            return "rpc";
        case FailureSource::Transport:
            return "transport";
        case FailureSource::Discovery:
            return "discovery";
        case FailureSource::Scheduler:
            return "scheduler";
        case FailureSource::Config:
            return "config";
        case FailureSource::Security:
            return "security";
        case FailureSource::DurableStore:
            return "durable_store";
        case FailureSource::Supervision:
            return "supervision";
        case FailureSource::Cluster:
            return "cluster";
        case FailureSource::Unknown:
            return "unknown";
    }
    return "unknown";
}

} // namespace hpactor
```

- [ ] **Step 2: Register in src/CMakeLists.txt**

Add `types/failure_reason.cpp` to the `add_library(hpactor_lib SHARED` block, after line 40 (`types/typed_message.cpp`):

```cmake
    types/failure_reason.cpp
```

- [ ] **Step 3: Build to verify compilation**

```bash
cd .worktrees/failure-envelope-spec && cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tail -5 && ninja -C build hpactor_lib 2>&1 | tail -10
```
Expected: Build succeeds with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/types/failure_reason.cpp src/CMakeLists.txt
git commit -m "feat(types): add to_string() tables for FailureReason and FailureSource"
```

---

### Task 3: Create `FailureEnvelope` struct

**Files:**
- Create: `include/hpactor/types/failure_envelope.hpp`

- [ ] **Step 1: Write the header**

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

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <array>
#include <cstdint>
#include <cstring>

namespace hpactor {

/// Shared failure carrier. Built at the point where a failure is first
/// detected and pushed to observability paths (log, trace, metric, DLQ).
struct FailureEnvelope {
    /// Canonical failure reason.
    FailureReason reason{FailureReason::Unknown};

    /// Target actor the failure relates to.
    ActorId actor_id{};

    /// Original sender address.
    ActorAddress sender{};

    /// Intended receiver address.
    ActorAddress receiver{};

    /// Message that failed (zeroed for non-message failures like spawn).
    MessageId message_id{};

    /// Distributed trace context at the failure point.
    TraceContext trace{};

    /// True if the caller can retry with a reasonable chance of success.
    bool retryable{false};

    /// Monotonic timestamp (nanoseconds) when the failure was recorded.
    uint64_t timestamp_ns{0};

    /// Which subsystem produced this failure.
    FailureSource source{FailureSource::ActorRuntime};

    /// Human-readable detail. Bounded to keep the envelope stack-friendly.
    std::array<char, 256> detail{};
    uint8_t detail_len{0};

    /// Set the detail string, truncating to the array capacity.
    /// Safe: max stored length is 255 (array size minus 1).
    void set_detail(std::string_view s) noexcept {
        detail_len = static_cast<uint8_t>(
            std::min(s.size(), detail.size() - 1));
        std::memcpy(detail.data(), s.data(), detail_len);
    }

    /// View of the detail string.
    [[nodiscard]] std::string_view detail_view() const noexcept {
        return {detail.data(), detail_len};
    }
};

/// Factory: fill an envelope with the fields available at the delivery
/// boundary. Timestamp is sampled from the caller's clock.
FailureEnvelope make_failure_envelope(FailureReason reason,
                                      ActorId actor_id,
                                      const ActorAddress& sender,
                                      const ActorAddress& receiver,
                                      MessageId message_id,
                                      const TraceContext& trace,
                                      FailureSource source,
                                      std::string_view detail = {}) noexcept;

} // namespace hpactor
```

- [ ] **Step 2: Verify types.hpp provides everything needed**

The header includes `<hpactor/types/types.hpp>` which must supply `ActorId`, `MessageId`, `TraceContext`. These are already defined there. Verify:

```bash
grep -n "ActorId\|MessageId\|TraceContext" include/hpactor/types/types.hpp | head -10
```
Expected: Shows definitions for all three types.

- [ ] **Step 3: Verify the header compiles in isolation**

```bash
cd .worktrees/failure-envelope-spec && echo '#include <hpactor/types/failure_envelope.hpp>' | g++ -std=c++20 -fsyntax-only -I include -x c++ -
```
Expected: No output (compiles clean).

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/types/failure_envelope.hpp
git commit -m "feat(types): add FailureEnvelope struct with factory function"
```

---

### Task 4: Implement `make_failure_envelope()` factory

**Files:**
- Modify: `src/types/failure_reason.cpp`

- [ ] **Step 1: Add the factory implementation**

Append to `src/types/failure_reason.cpp`:

```cpp
#include <hpactor/types/failure_envelope.hpp>

#include <chrono>

namespace hpactor {

FailureEnvelope make_failure_envelope(FailureReason reason,
                                      ActorId actor_id,
                                      const ActorAddress& sender,
                                      const ActorAddress& receiver,
                                      MessageId message_id,
                                      const TraceContext& trace,
                                      FailureSource source,
                                      std::string_view detail) noexcept {
    FailureEnvelope env;
    env.reason = reason;
    env.actor_id = actor_id;
    env.sender = sender;
    env.receiver = receiver;
    env.message_id = message_id;
    env.trace = trace;
    env.retryable = retryable(reason);
    env.source = source;
    if (!detail.empty()) {
        env.set_detail(detail);
    }
    // Sample monotonic timestamp
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    env.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return env;
}

} // namespace hpactor
```

- [ ] **Step 2: Build to verify**

```bash
cd .worktrees/failure-envelope-spec && ninja -C build hpactor_lib 2>&1 | tail -10
```
Expected: Build succeeds (may need to re-run cmake if new include deps changed).

- [ ] **Step 3: Commit**

```bash
git add src/types/failure_reason.cpp
git commit -m "feat(types): implement make_failure_envelope() factory"
```

---

### Task 5: Add `failure_reason()` to `EnqueueResult` and `EnqueueResultCode`

**Files:**
- Modify: `include/hpactor/mailbox/mailbox_policy.hpp`

- [ ] **Step 1: Add a new `#include` and the mapping free function**

Add `#include <hpactor/types/failure_reason.hpp>` after line 19 (`#include <hpactor/types/types.hpp>`).

Then, after the `EnqueueResultCode` enum (after line 103), add:

```cpp
/// Map an EnqueueResultCode to the canonical FailureReason.
/// Accepted and AcceptedWithSoftPressure are not failures — callers
/// should guard with !result.accepted() before calling.
[[nodiscard]] constexpr FailureReason
failure_reason(EnqueueResultCode code) noexcept {
    switch (code) {
        case EnqueueResultCode::Rejected:
            return FailureReason::RejectedByPolicy;
        case EnqueueResultCode::DroppedNewest:
        case EnqueueResultCode::DroppedExisting:
            return FailureReason::Dropped;
        case EnqueueResultCode::ReroutedToDeadLetter:
            return FailureReason::RejectedByPolicy;
        case EnqueueResultCode::ReroutedToOverflow:
            return FailureReason::RejectedByPolicy;
        case EnqueueResultCode::MailboxClosed:
            return FailureReason::MailboxClosed;
        case EnqueueResultCode::ActorNotFound:
            return FailureReason::NoRoute;
        case EnqueueResultCode::Accepted:
        case EnqueueResultCode::AcceptedWithSoftPressure:
            return FailureReason::Unknown; // Not a failure
    }
    return FailureReason::Unknown;
}
```

- [ ] **Step 2: Add `failure_reason()` method to `EnqueueResult`**

Add after line 131 (the closing `};` of the `retryable()` method in `EnqueueResult`):

```cpp
    /// Canonical failure reason for this result.
    /// Returns Unknown when the enqueue was accepted.
    [[nodiscard]] FailureReason failure_reason() const noexcept {
        return mailbox::failure_reason(code);
    }
```

- [ ] **Step 3: Verify the enum values compile**

```bash
cd .worktrees/failure-envelope-spec && echo '#include <hpactor/mailbox/mailbox_policy.hpp>
int main() {
    using namespace hpactor::mailbox;
    auto r = failure_reason(EnqueueResultCode::ActorNotFound);
    (void)r;
    return 0;
}' | g++ -std=c++20 -I include -x c++ - -o /dev/null
```
Expected: Compiles clean with no output.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mailbox/mailbox_policy.hpp
git commit -m "feat(mailbox): add failure_reason mapping from EnqueueResultCode"
```

---

### Task 6: Add `failure_reason()` to `error` class

**Files:**
- Modify: `include/hpactor/types/types.hpp` (lines 254-278, the `error` class)

- [ ] **Step 1: Add the `#include` and `failure_reason()` method**

Add `#include <hpactor/types/failure_reason.hpp>` after the existing includes (after line 10 or wherever the last include is at the top of types.hpp).

Add a public method to the `error` class after `bool ok()` (after line 270):

```cpp
    /// Map this error's code to the canonical FailureReason.
    /// Returns FailureReason::Unknown when no mapping exists.
    [[nodiscard]] FailureReason failure_reason() const noexcept {
        switch (code_) {
            case errors::unknown:       return FailureReason::Unknown;
            case errors::actor_down:    return FailureReason::ActorDead;
            case errors::actor_not_found: return FailureReason::NoRoute;
            case errors::mailbox_full:  return FailureReason::MailboxFull;
            case errors::timeout:       return FailureReason::Timeout;
            case errors::invalid_argument: return FailureReason::RejectedByPolicy;
            default:                    return FailureReason::Unknown;
        }
    }
```

- [ ] **Step 2: Verify compilation**

```bash
cd .worktrees/failure-envelope-spec && echo '#include <hpactor/types/types.hpp>
int main() {
    hpactor::error err(hpactor::errors::timeout, "timed out");
    auto r = err.failure_reason();
    (void)r;
    return 0;
}' | g++ -std=c++20 -I include -x c++ - -o /dev/null
```
Expected: Compiles clean.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/types/types.hpp
git commit -m "feat(types): add failure_reason() mapping to error class"
```

---

### Task 7: Add `kDeliveryFailure` metric event type

**Files:**
- Modify: `include/hpactor/metrics/metrics_event.hpp` (line 43)

- [ ] **Step 1: Append new enum value**

Add after `kActorDrainTimeout = 19,`:

```cpp
    kDeliveryFailure = 20,
```

- [ ] **Step 2: Verify compilation**

```bash
cd .worktrees/failure-envelope-spec && ninja -C build hpactor_lib 2>&1 | tail -5
```
Expected: Build succeeds (enum extension is ABI-safe since it's uint8_t backing).

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/metrics/metrics_event.hpp
git commit -m "feat(metrics): add kDeliveryFailure metric event type"
```

---

### Task 8: Wire `FailureEnvelope` into `try_deliver_local()` failure paths

**Background:** `try_deliver_local()` is a method of `ActorSystem`. The private
members `metrics_ring_buffer_` (`shared_ptr<MpscRingBuffer<MetricEvent>>`) and
`logger_` (`log::Logger*`) are available directly. The public getter
`metrics_ring_buffer()` returns `metrics_ring_buffer_.get()` (line 278 of
`actor_system.hpp`).

**Files:**
- Modify: `src/actor/actor_system.cpp` (lines 393-465, the `try_deliver_local` method)

- [ ] **Step 1: Add includes**

Add after the existing includes at the top of `actor_system.cpp`:

```cpp
#include <hpactor/types/failure_envelope.hpp>
```

- [ ] **Step 2: Build FailureEnvelope on ActorNotFound path**

In `try_deliver_local()`, after the `r.code = mailbox::EnqueueResultCode::ActorNotFound;` line (currently line 417) and before `return r;` (line 418), add:

```cpp
        // Build failure envelope for observability
        {
            FailureEnvelope env = make_failure_envelope(
                FailureReason::NoRoute,
                target,
                msg.sender_address(),
                ActorAddress{endpoint_, ActorType{0}, target, 0},
                MessageId{options.message_id},
                msg.trace_context(),
                FailureSource::ActorRuntime,
                "target actor not found in registry");
            // Emit metric event
            if (metrics_ring_buffer_) [[unlikely]] {
                metrics::MetricEvent evt{};
                evt.timestamp_ns = env.timestamp_ns;
                evt.actor_id = target;
                evt.event_type = metrics::MetricEventType::kDeliveryFailure;
                evt.code = static_cast<uint8_t>(env.reason);
                evt.value_hi = 1;
                metrics_ring_buffer_->try_push(evt);
            }
            // Structured log
            if (logger_) [[unlikely]] {
                HPACTOR_LOG_WARNING(log::LogCategory::kActor,
                    target, 0,
                    "delivery_failure: %s sender=%s reason=no_route "
                    "retryable=%s",
                    msg.type_name(),
                    msg.sender_address().to_string().c_str(),
                    env.retryable ? "true" : "false");
            }
        }
```

> **Note on `msg.trace_context()`:** If `TypedMessage` does not have a
> `trace_context()` accessor, use `TraceContext{}` and add a TODO comment:
> `// TODO: plumb trace context from TypedMessage envelope`.

> **Note on `msg.sender_address().to_string()`:** If `ActorAddress` does not
> have a `to_string()` method, check the existing format (e.g., `endpoint_ops::to_string`).

- [ ] **Step 3: Build FailureEnvelope on mailbox rejection path**

In `try_deliver_local()`, after the `!result.accepted()` dead-letter block (currently lines 435-452, the `if (!result.accepted() && ...)` block) and before the backpressure check, add:

```cpp
    if (!result.accepted()) {
        FailureEnvelope env = make_failure_envelope(
            result.failure_reason(),
            target,
            msg.sender_address(),
            ActorAddress{endpoint_, ActorType{0}, target, 0},
            MessageId{options.message_id},
            msg.trace_context(),
            FailureSource::Mailbox,
            "mailbox admission rejected");
        if (metrics_ring_buffer_) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.timestamp_ns = env.timestamp_ns;
            evt.actor_id = target;
            evt.event_type = metrics::MetricEventType::kDeliveryFailure;
            evt.code = static_cast<uint8_t>(env.reason);
            evt.value_hi = 1;
            metrics_ring_buffer_->try_push(evt);
        }
        if (logger_) [[unlikely]] {
            HPACTOR_LOG_WARNING(log::LogCategory::kActor,
                target, 0,
                "delivery_failure: %s reason=%s retryable=%s "
                "depth=%u capacity=%u",
                msg.type_name(),
                to_string(env.reason),
                env.retryable ? "true" : "false",
                result.depth, result.capacity);
        }
    }
```

- [ ] **Step 4: Verify the TypedMessage API**

Check that `TypedMessage` has `trace_context()` and `sender_address()` methods:

```bash
grep -n "trace_context\|sender_address" include/hpactor/actor/typed_message.hpp
```

If not present, adjust the code to use whatever trace/sender accessors exist on `TypedMessage`, or fall back to `TraceContext{}` for tracing and `msg.sender_address()` for sender.

- [ ] **Step 5: Build to verify**

```bash
cd .worktrees/failure-envelope-spec && ninja -C build hpactor_lib 2>&1 | tail -15
```
Expected: Build succeeds. Fix any compilation errors (missing accessors, wrong method names).

- [ ] **Step 6: Commit**

```bash
git add src/actor/actor_system.cpp
git commit -m "feat(actor): wire FailureEnvelope into try_deliver_local failure paths"
```

---

### Task 9: Write unit tests — `FailureReason` and mapping

**Files:**
- Create: `tests/core/test_failure_reason.cpp`

- [ ] **Step 1: Write the test file**

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

#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <cstdlib>
#include <cstring>

// Always-on assertion (NDEBUG strips standard assert in Release builds).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

int main() {
    // ── retryable() ─────────────────────────────────────────────────
    CHECK(hpactor::retryable(hpactor::FailureReason::NoRoute));
    CHECK(hpactor::retryable(hpactor::FailureReason::NodeUnavailable));
    CHECK(!hpactor::retryable(hpactor::FailureReason::ActorDead));
    CHECK(hpactor::retryable(hpactor::FailureReason::ActorNotReady));
    CHECK(!hpactor::retryable(hpactor::FailureReason::Quarantined));
    CHECK(hpactor::retryable(hpactor::FailureReason::CircuitOpen));
    CHECK(hpactor::retryable(hpactor::FailureReason::MailboxFull));
    CHECK(hpactor::retryable(hpactor::FailureReason::OutboundQueueFull));
    CHECK(hpactor::retryable(hpactor::FailureReason::MemoryPressure));
    CHECK(!hpactor::retryable(hpactor::FailureReason::Expired));
    CHECK(hpactor::retryable(hpactor::FailureReason::Timeout));
    CHECK(!hpactor::retryable(hpactor::FailureReason::RejectedByPolicy));
    CHECK(!hpactor::retryable(hpactor::FailureReason::Dropped));
    CHECK(!hpactor::retryable(hpactor::FailureReason::MailboxClosed));
    CHECK(!hpactor::retryable(hpactor::FailureReason::SerializationError));
    CHECK(hpactor::retryable(hpactor::FailureReason::TransportError));
    CHECK(!hpactor::retryable(hpactor::FailureReason::FrameRejected));
    CHECK(!hpactor::retryable(hpactor::FailureReason::Duplicate));
    CHECK(hpactor::retryable(hpactor::FailureReason::Draining));
    CHECK(hpactor::retryable(hpactor::FailureReason::ShuttingDown));
    CHECK(!hpactor::retryable(hpactor::FailureReason::RetryExhausted));
    CHECK(!hpactor::retryable(hpactor::FailureReason::SpawnFailed));
    CHECK(!hpactor::retryable(hpactor::FailureReason::Unknown));

    // ── to_string(FailureReason) ────────────────────────────────────
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::NoRoute),
                      "no_route") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::ActorDead),
                      "actor_dead") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::MailboxFull),
                      "mailbox_full") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::Timeout),
                      "timeout") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::Unknown),
                      "unknown") == 0);

    // All 24 values must return non-null strings
    for (uint8_t i = 0; i <= 90; ++i) {
        auto r = static_cast<hpactor::FailureReason>(i);
        const char* s = hpactor::to_string(r);
        CHECK(s != nullptr);
        CHECK(std::strlen(s) > 0);
    }
    // Sentinel
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::Unknown),
                      "unknown") == 0);

    // ── to_string(FailureSource) ────────────────────────────────────
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureSource::ActorRuntime),
                      "actor_runtime") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureSource::Mailbox),
                      "mailbox") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureSource::Unknown),
                      "unknown") == 0);

    // ── EnqueueResultCode → FailureReason mapping ───────────────────
    using namespace hpactor::mailbox;
    CHECK(failure_reason(EnqueueResultCode::Accepted) ==
          hpactor::FailureReason::Unknown);
    CHECK(failure_reason(EnqueueResultCode::AcceptedWithSoftPressure) ==
          hpactor::FailureReason::Unknown);
    CHECK(failure_reason(EnqueueResultCode::Rejected) ==
          hpactor::FailureReason::RejectedByPolicy);
    CHECK(failure_reason(EnqueueResultCode::DroppedNewest) ==
          hpactor::FailureReason::Dropped);
    CHECK(failure_reason(EnqueueResultCode::DroppedExisting) ==
          hpactor::FailureReason::Dropped);
    CHECK(failure_reason(EnqueueResultCode::ReroutedToDeadLetter) ==
          hpactor::FailureReason::RejectedByPolicy);
    CHECK(failure_reason(EnqueueResultCode::ReroutedToOverflow) ==
          hpactor::FailureReason::RejectedByPolicy);
    CHECK(failure_reason(EnqueueResultCode::MailboxClosed) ==
          hpactor::FailureReason::MailboxClosed);
    CHECK(failure_reason(EnqueueResultCode::ActorNotFound) ==
          hpactor::FailureReason::NoRoute);

    // ── EnqueueResult::failure_reason() method ──────────────────────
    {
        EnqueueResult r;
        r.code = EnqueueResultCode::ActorNotFound;
        CHECK(r.failure_reason() == hpactor::FailureReason::NoRoute);
    }
    {
        EnqueueResult r;
        r.code = EnqueueResultCode::Accepted;
        CHECK(!r.accepted() || r.failure_reason() ==
                                 hpactor::FailureReason::Unknown);
    }

    // ── error::failure_reason() mapping ─────────────────────────────
    {
        hpactor::error err(hpactor::errors::actor_down, "down");
        CHECK(err.failure_reason() == hpactor::FailureReason::ActorDead);
    }
    {
        hpactor::error err(hpactor::errors::actor_not_found, "missing");
        CHECK(err.failure_reason() == hpactor::FailureReason::NoRoute);
    }
    {
        hpactor::error err(hpactor::errors::mailbox_full, "full");
        CHECK(err.failure_reason() == hpactor::FailureReason::MailboxFull);
    }
    {
        hpactor::error err(hpactor::errors::timeout, "timeout");
        CHECK(err.failure_reason() == hpactor::FailureReason::Timeout);
    }
    {
        hpactor::error err(hpactor::errors::invalid_argument, "bad arg");
        CHECK(err.failure_reason() ==
              hpactor::FailureReason::RejectedByPolicy);
    }
    {
        hpactor::error err(hpactor::errors::unknown, "?");
        CHECK(err.failure_reason() == hpactor::FailureReason::Unknown);
    }
    {
        hpactor::error err(9999, "unmapped");
        CHECK(err.failure_reason() == hpactor::FailureReason::Unknown);
    }

    return 0;
}
```

- [ ] **Step 2: Register test in tests/CMakeLists.txt**

Add after the `test_types` and `test_result` block (after line 27):

```cmake
add_executable(test_failure_reason core/test_failure_reason.cpp)
target_link_libraries(test_failure_reason hpactor)
add_test(NAME test_failure_reason COMMAND test_failure_reason)
```

- [ ] **Step 3: Build and run the test**

```bash
cd .worktrees/failure-envelope-spec && cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tail -3 && ninja -C build test_failure_reason 2>&1 | tail -5 && ./build/tests/test_failure_reason
```
Expected: Test exits with code 0 (no output = all CHECKs pass).

- [ ] **Step 4: Commit**

```bash
git add tests/core/test_failure_reason.cpp tests/CMakeLists.txt
git commit -m "test: add FailureReason/mapping unit tests"
```

---

### Task 10: Write unit tests — `FailureEnvelope`

**Files:**
- Create: `tests/core/test_failure_envelope.cpp`

- [ ] **Step 1: Write the test file**

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

#include <hpactor/types/failure_envelope.hpp>

#include <cstdlib>
#include <cstring>

// Always-on assertion (NDEBUG strips standard assert in Release builds).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

int main() {
    // ── Default-constructed envelope ────────────────────────────────
    {
        hpactor::FailureEnvelope env;
        CHECK(env.reason == hpactor::FailureReason::Unknown);
        CHECK(env.actor_id == hpactor::ActorId{});
        CHECK(env.message_id == hpactor::MessageId{});
        CHECK(env.retryable == false);
        CHECK(env.timestamp_ns == 0);
        CHECK(env.detail_len == 0);
        CHECK(env.source == hpactor::FailureSource::ActorRuntime);
    }

    // ── make_failure_envelope() fills all fields ────────────────────
    {
        hpactor::ActorId target_id{42};
        hpactor::ActorAddress sender_addr;
        hpactor::ActorAddress receiver_addr;
        hpactor::MessageId msg_id{100};
        hpactor::TraceContext trace_ctx;

        auto env = hpactor::make_failure_envelope(
            hpactor::FailureReason::MailboxFull,
            target_id,
            sender_addr,
            receiver_addr,
            msg_id,
            trace_ctx,
            hpactor::FailureSource::Mailbox,
            "depth=1024 capacity=1024");

        CHECK(env.reason == hpactor::FailureReason::MailboxFull);
        CHECK(env.actor_id == target_id);
        CHECK(env.message_id == msg_id);
        CHECK(env.retryable == true); // MailboxFull is retryable
        CHECK(env.timestamp_ns > 0);
        CHECK(env.source == hpactor::FailureSource::Mailbox);
        CHECK(env.detail_len > 0);
        CHECK(std::strncmp(env.detail.data(), "depth=1024",
                           env.detail_len) == 0);
        CHECK(env.detail_view() == "depth=1024 capacity=1024");
    }

    // ── Non-retryable reason ────────────────────────────────────────
    {
        hpactor::ActorId target{1};
        hpactor::ActorAddress addr;
        hpactor::MessageId mid;
        hpactor::TraceContext tc;

        auto env = hpactor::make_failure_envelope(
            hpactor::FailureReason::ActorDead,
            target, addr, addr, mid, tc,
            hpactor::FailureSource::ActorRuntime, "");

        CHECK(env.reason == hpactor::FailureReason::ActorDead);
        CHECK(env.retryable == false);
        CHECK(env.detail_len == 0);
    }

    // ── set_detail() truncation ─────────────────────────────────────
    {
        hpactor::FailureEnvelope env;
        // Build a string longer than 256 chars
        std::string long_str(300, 'x');
        env.set_detail(long_str);
        CHECK(env.detail_len == 255); // array size 256, min(300,256) = 256…
        // Actually min(300, 256) = 256, but detail_len is uint8_t so
        // it stores 256 as 0 (overflow). The set_detail uses
        // min(s.size(), detail.size()) which is 256, stored as uint8_t
        // = 0. Let's use 255 as the actual max.
        // Re-test with exactly 255:
    }
    {
        hpactor::FailureEnvelope env;
        std::string str255(255, 'y');
        env.set_detail(str255);
        CHECK(env.detail_len == 255);
    }

    // ── retryable flag matches retryable(FailureReason) ─────────────
    {
        hpactor::ActorId id{1};
        hpactor::ActorAddress a;
        hpactor::MessageId m;
        hpactor::TraceContext t;

        auto env = hpactor::make_failure_envelope(
            hpactor::FailureReason::Timeout,
            id, a, a, m, t,
            hpactor::FailureSource::ActorRuntime, "");

        CHECK(env.retryable == hpactor::retryable(env.reason));
    }

    return 0;
}
```

- [ ] **Step 2: Fix `set_detail()` overflow**

The `set_detail()` method uses `uint8_t` for `detail_len` but `std::min(s.size(), detail.size())` returns `size_t{256}` when the string is 256 chars, which doesn't fit in `uint8_t`. The array is 256 bytes, so the max indexable is 255. Fix `set_detail()` in `failure_envelope.hpp`:

```cpp
    void set_detail(std::string_view s) noexcept {
        detail_len = static_cast<uint8_t>(
            std::min(s.size(), detail.size() - 1));
        std::memcpy(detail.data(), s.data(), detail_len);
    }
```

- [ ] **Step 3: Register test in tests/CMakeLists.txt**

Add after the `test_failure_reason` block:

```cmake
add_executable(test_failure_envelope core/test_failure_envelope.cpp)
target_link_libraries(test_failure_envelope hpactor)
add_test(NAME test_failure_envelope COMMAND test_failure_envelope)
```

- [ ] **Step 4: Build and run**

```bash
cd .worktrees/failure-envelope-spec && ninja -C build test_failure_envelope 2>&1 | tail -5 && ./build/tests/test_failure_envelope
```
Expected: Test exits with code 0.

- [ ] **Step 5: Commit**

```bash
git add tests/core/test_failure_envelope.cpp tests/CMakeLists.txt include/hpactor/types/failure_envelope.hpp
git commit -m "test: add FailureEnvelope unit tests and fix set_detail overflow"
```

---

### Task 11: Full build and test run

**Files:** None (verification only)

- [ ] **Step 1: Full build**

```bash
cd .worktrees/failure-envelope-spec && cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tail -3 && ninja -C build 2>&1 | tail -10
```
Expected: Full build succeeds with all targets.

- [ ] **Step 2: Run all tests**

```bash
cd .worktrees/failure-envelope-spec && ctest --output-on-failure --parallel 8 2>&1 | tail -20
```
Expected: All existing tests pass, plus the two new test executables.

- [ ] **Step 3: Run new tests explicitly**

```bash
cd .worktrees/failure-envelope-spec && ./build/tests/test_failure_reason && echo "PASS: test_failure_reason" && ./build/tests/test_failure_envelope && echo "PASS: test_failure_envelope"
```
Expected: Both print PASS.

---

### Task 12: Final commit — update CLUEDE_MEMORY.md

**Files:**
- Modify: `CLAUDE_MEMORY.md`

- [ ] **Step 1: Update project memory with Phase 1 completion status**

Read `CLAUDE_MEMORY.md` and add a note under the current status section:

```markdown
### 2026-05-20: Failure Envelope Phase 1 Types

- `FailureReason` enum (24 values, 10 ranges) + `FailureSource` enum added to
  `include/hpactor/types/failure_reason.hpp`.
- `FailureEnvelope` struct + `make_failure_envelope()` factory added to
  `include/hpactor/types/failure_envelope.hpp`.
- `EnqueueResult` and `error` class gain `failure_reason()` mapping methods.
- `try_deliver_local()` builds a `FailureEnvelope` on every failure path and
  emits `kDeliveryFailure` metric event.
- Phase 2 (DLQ integration), Phase 3 (RPC + spawn), Phase 4 (CLI) remain.
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE_MEMORY.md
git commit -m "docs: update project memory with failure envelope Phase 1 status"
```

---

## Verification Checklist

Before declaring the branch done, verify:

- [ ] `ninja -C build` — zero errors, zero warnings
- [ ] `ctest --output-on-failure --parallel 8` — all 152 tests pass (150 existing + 2 new)
- [ ] `./build/tests/test_failure_reason` — exits 0
- [ ] `./build/tests/test_failure_envelope` — exits 0
- [ ] `git log --oneline` — clean commit history, each task is one commit
- [ ] `git diff main --stat` — only the expected files changed

---

## Deferred to Follow-On Tasks

These items from the design spec are intentionally deferred past Phase 1:

| Item | Reason | Target |
|------|--------|--------|
| `error` class gains `unique_ptr<FailureEnvelope>` | Dependency cycle between `types.hpp` and `failure_envelope.hpp`. The `failure_reason()` mapping achieves the core goal (shared reason vocabulary) without the complexity. Add the envelope pointer when a concrete use case requires it. | Phase 2 |
| Trace span attributes (`failure.reason`, `failure.source`) | Requires plumbing trace context from `TypedMessage` through the delivery path. The `FailureEnvelope` is built with `TraceContext{}` placeholder. | Phase 2 |
| DLQ record uses `FailureReason`/`FailureEnvelope` | The `DeadLetterRecord` struct in `dead_letter_queue.hpp` has its own `DeadLetterReason` enum. Mapping it to `FailureReason` is the Phase 2 task. | Phase 2 |
| RPC and Spawn error integration | These paths are in separate subsystems. Phase 3 in the design spec. | Phase 3 |
| CLI `/failure` commands | Requires a recent-failure ring buffer or query model. Phase 4. | Phase 4 |

## Self-Review Notes

- **Spec coverage:** All 7 Phase 1 deliverables are covered by tasks: types (T1-T4), mapping (T5-T6), metric event (T7), wiring (T8), tests (T9-T10), build verification (T11), memory update (T12).
- **Deferred items:** Two Phase 1 items from the design spec — `error::envelope()` accessor and trace span attributes — are explicitly deferred above with rationale.
- **Type consistency:** `FailureReason` enum values, `FailureEnvelope` field names, method signatures (`failure_reason()`, `retryable()`, `to_string()`, `make_failure_envelope()`) are consistent across all 12 tasks.
- **No placeholders:** All code steps contain complete, compilable code. The two areas that depend on runtime API discovery (`msg.trace_context()`, `msg.sender_address().to_string()`) have explicit fallback instructions in Task 8 Step 2 notes.
- **`set_detail()` overflow:** Caught in Task 10 Step 2 and fixed in Task 3 Step 1 — uses `detail.size() - 1` (255) to avoid uint8_t overflow from value 256.
