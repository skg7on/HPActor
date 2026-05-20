# Shared ADT Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract common user-defined data structures across HPActor into shared C++20 template-based ADTs, eliminating code duplication while preserving compile-time type safety and zero runtime overhead.

**Architecture:** Five independent sections implemented sequentially. Each section is self-contained and buildable. Section 1 (Id template) establishes the ADT convention. Sections 2-5 each address a distinct duplication pattern. New files go in `include/hpactor/adt/`; X-macro tables in `include/hpactor/config/`.

**Tech Stack:** C++20 templates, X-macro preprocessor, header-only ADTs, no RTTI/exceptions

---

### Task 1: Create `Id<Tag, T>` template and tag types

**Files:**
- Create: `include/hpactor/adt/id.hpp`
- Create: `include/hpactor/adt/tags.hpp`

- [ ] **Step 1: Write `include/hpactor/adt/tags.hpp`**

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

namespace hpactor {

struct ActorTag {};
struct MessageTag {};
struct AlarmTag {};

} // namespace hpactor

namespace hpactor::sched {

struct TimerTag {};

} // namespace hpactor::sched
```

- [ ] **Step 2: Write `include/hpactor/adt/id.hpp`**

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

#include <cstddef>
#include <cstdint>
#include <functional>

namespace hpactor {

template <typename Tag, typename T = uint64_t>
class Id {
    T value_{};
public:
    constexpr Id() = default;
    explicit constexpr Id(T v) : value_{v} {}

    [[nodiscard]] constexpr T value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != T{}; }

    friend constexpr bool operator==(Id, Id) = default;
    friend constexpr bool operator!=(Id, Id) = default;
    friend constexpr auto operator<=>(Id, Id) = default;
};

} // namespace hpactor

template <typename Tag, typename T>
struct std::hash<hpactor::Id<Tag, T>> {
    std::size_t operator()(hpactor::Id<Tag, T> id) const noexcept {
        return std::hash<T>{}(id.value());
    }
};
```

- [ ] **Step 3: Build to verify compilation**

```bash
cd /Users/skg7on/Workspace/Projects/HPActor/.worktrees/shared-adt-extraction
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tail -5
ninja -C build 2>&1 | tail -5
```

Expected: build succeeds (new headers not yet included by anything, no-op).

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/adt/id.hpp include/hpactor/adt/tags.hpp
git commit -m "feat(adt): add Id<Tag, T> opaque identifier template and tag types"
```

---

### Task 2: Replace ActorId/MessageId/AlarmHandle with aliases

**Files:**
- Modify: `include/hpactor/types/types.hpp:44-66` (ActorId), `:250-281` (MessageId), `:399-412` (AlarmHandle)

- [ ] **Step 1: Replace ActorId class definition with alias**

In `include/hpactor/types/types.hpp`, replace lines 44-66 (the entire `struct ActorId { ... };` block):

```cpp
// -----------------------------------------------------------------------------
// ActorId - unique identifier for an actor instance
// -----------------------------------------------------------------------------
using ActorId = Id<ActorTag>;
```

- [ ] **Step 2: Replace MessageId class definition with alias and free function**

In `include/hpactor/types/types.hpp`, replace lines 250-281 (the entire `struct MessageId { ... };` block plus the `inline std::atomic` definition):

```cpp
// -----------------------------------------------------------------------------
// MessageId - unique identifier for a message
// -----------------------------------------------------------------------------
using MessageId = Id<MessageTag>;

inline MessageId generate_message_id() {
    static std::atomic<uint64_t> next_id_{1};
    return MessageId(next_id_.fetch_add(1));
}
```

Add `#include <hpactor/adt/id.hpp>` and `#include <hpactor/adt/tags.hpp>` at the top of the include block (after the existing `#include <hpactor/adt/stream_buffer.hpp>`).

- [ ] **Step 3: Replace AlarmHandle class definition with alias**

In `include/hpactor/types/types.hpp`, replace lines 399-412 (the entire `struct AlarmHandle { ... };` block):

```cpp
// -----------------------------------------------------------------------------
// AlarmHandle - opaque handle for alarms
// -----------------------------------------------------------------------------
using AlarmHandle = Id<AlarmTag>;
```

- [ ] **Step 4: Build and fix any compilation errors**

```bash
cd /Users/skg7on/Workspace/Projects/HPActor/.worktrees/shared-adt-extraction
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tail -5
ninja -C build 2>&1 | tail -20
```

Expected: build succeeds. The `using` aliases preserve all call sites. `Id` provides `value()`, `valid()`, `==`, `!=`, `<=>` — matching the old interfaces. `AlarmHandle::id()` becomes `AlarmHandle::value()` via the alias, so if any code calls `.id()` directly on `AlarmHandle`, those will need updating to `.value()`. Check for this.

If compilation fails due to `.id()` calls on AlarmHandle, fix each call site: replace `handle.id()` with `handle.value()`.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/types/types.hpp
git commit -m "refactor(adt): replace ActorId/MessageId/AlarmHandle with Id<Tag> aliases"
```

---

### Task 3: Replace TimerHandle with alias

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp:56-60`

- [ ] **Step 1: Replace TimerHandle struct with alias**

In `include/hpactor/sched/scheduler.hpp`, find the `struct TimerHandle` block and replace it:

```cpp
// -----------------------------------------------------------------------------
// TimerHandle — opaque handle for scheduled timers
// -----------------------------------------------------------------------------
using TimerHandle = Id<TimerTag>;
```

Add `#include <hpactor/adt/id.hpp>` and `#include <hpactor/adt/tags.hpp>` to the scheduler header.

- [ ] **Step 2: Build and verify**

```bash
ninja -C build 2>&1 | tail -20
```

Expected: build succeeds. TimerHandle provides `valid()` and `==`/`!=` through `Id`.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/sched/scheduler.hpp
git commit -m "refactor(adt): replace TimerHandle with Id<TimerTag> alias"
```

---

### Task 4: Replace MessageId::generate() call sites

**Files:**
- Modify: `src/ref/actor_proxy.cpp:98`
- Modify: `src/actor/actor_system.cpp:543`
- Modify: `src/rpc/rpc_channel.cpp:145`
- Modify: `tests/tracing/test_trace_rpc.cpp:20`
- Modify: `tests/core/test_types.cpp:78-79`
- Modify: `tests/spawn/test_spawn_integration.cpp:78`

- [ ] **Step 1: Update each call site**

Replace `MessageId::generate()` with `generate_message_id()` at each location:

`src/ref/actor_proxy.cpp:98`:
```cpp
// Before:
frame.pb_frame.set_message_id(MessageId::generate().value());
// After:
frame.pb_frame.set_message_id(generate_message_id().value());
```

`src/actor/actor_system.cpp:543`:
```cpp
// Before:
uint64_t msg_id = MessageId::generate().value();
// After:
uint64_t msg_id = generate_message_id().value();
```

`src/rpc/rpc_channel.cpp:145`:
```cpp
// Before:
MessageId msg_id = MessageId::generate();
// After:
MessageId msg_id = generate_message_id();
```

`tests/tracing/test_trace_rpc.cpp:20`:
```cpp
// Before:
call.msg_id = MessageId::generate();
// After:
call.msg_id = generate_message_id();
```

`tests/core/test_types.cpp:78-79`:
```cpp
// Before:
hpactor::MessageId id1 = hpactor::MessageId::generate();
hpactor::MessageId id2 = hpactor::MessageId::generate();
// After:
hpactor::MessageId id1 = hpactor::generate_message_id();
hpactor::MessageId id2 = hpactor::generate_message_id();
```

`tests/spawn/test_spawn_integration.cpp:78`:
```cpp
// Before:
uint64_t request_message_id = hpactor::MessageId::generate().value();
// After:
uint64_t request_message_id = hpactor::generate_message_id().value();
```

- [ ] **Step 2: Build and run tests**

```bash
ninja -C build 2>&1 | tail -5
ctest --output-on-failure --parallel 8 2>&1 | tail -10
```

Expected: all 140 tests pass.

- [ ] **Step 3: Commit**

```bash
git add src/ref/actor_proxy.cpp src/actor/actor_system.cpp src/rpc/rpc_channel.cpp \
        tests/tracing/test_trace_rpc.cpp tests/core/test_types.cpp \
        tests/spawn/test_spawn_integration.cpp
git commit -m "refactor(adt): replace MessageId::generate() with generate_message_id()"
```

---

### Task 5: Add status()/ok() consistency to EnqueueResult and TraceParseResult

**Files:**
- Modify: `include/hpactor/mailbox/mailbox_policy.hpp:105-125`
- Modify: `include/hpactor/tracing/trace_context_parser.hpp:21-24`

- [ ] **Step 1: Add status() and ok() to EnqueueResult**

In `include/hpactor/mailbox/mailbox_policy.hpp`, add to `EnqueueResult` (after the `accepted()` method, around line 118):

```cpp
    [[nodiscard]] EnqueueResultCode status() const noexcept { return code; }

    [[nodiscard]] bool ok() const noexcept { return accepted(); }
```

- [ ] **Step 2: Add status() and ok() to TraceParseResult**

In `include/hpactor/tracing/trace_context_parser.hpp`, add to `TraceParseResult`:

```cpp
    [[nodiscard]] TraceParseStatus status() const noexcept { return status; }
    [[nodiscard]] bool ok() const noexcept { return status == TraceParseStatus::kOk; }
```

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build 2>&1 | tail -5
ctest --output-on-failure --parallel 8 2>&1 | tail -10
```

Expected: all 140 tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mailbox/mailbox_policy.hpp \
        include/hpactor/tracing/trace_context_parser.hpp
git commit -m "refactor(adt): add status()/ok() consistency to EnqueueResult and TraceParseResult"
```

---

### Task 6: Create NodeIdentity ADT and update structs

**Files:**
- Create: `include/hpactor/adt/node_identity.hpp`
- Modify: `include/hpactor/net/service_discovery.hpp:31-41`
- Modify: `include/hpactor/net/registrar.hpp:58-66`
- Modify: `include/hpactor/net/gossip_membership.hpp:41-49`

- [ ] **Step 1: Create `include/hpactor/adt/node_identity.hpp`**

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

#include <hpactor/net/acceptor.hpp>
#include <hpactor/types/types.hpp>

#include <string>
#include <vector>

namespace hpactor {

struct NodeIdentity {
    EndPoint endpoint;
    std::string host;
    std::string uds_path;
    std::vector<net::AcceptorInfo> acceptors;

    bool operator==(const NodeIdentity&) const = default;
};

} // namespace hpactor
```

- [ ] **Step 2: Update Member struct**

In `include/hpactor/net/service_discovery.hpp`, replace the Member struct fields:

```cpp
struct Member {
    NodeIdentity identity;
    std::vector<std::string> actor_types;
    MemberStatus status = MemberStatus::Alive;
    uint64_t incarnation = 0;
    std::chrono::steady_clock::time_point last_seen;
};
```

Add `#include <hpactor/adt/node_identity.hpp>` at the top.

- [ ] **Step 3: Update NodeEndpoint struct**

In `include/hpactor/net/registrar.hpp`, replace the NodeEndpoint struct:

```cpp
struct NodeEndpoint {
    NodeIdentity identity;
    uint16_t tcp_port = 0;
    bool is_static_route = false;
    std::chrono::steady_clock::time_point last_seen;
};
```

Add `#include <hpactor/adt/node_identity.hpp>` at the top.

- [ ] **Step 4: Update PiggybackEntry struct**

In `include/hpactor/net/gossip_membership.hpp`, replace the PiggybackEntry struct:

```cpp
struct PiggybackEntry {
    PiggybackType type;
    NodeIdentity identity;
    uint64_t incarnation;
    std::vector<std::string> actor_types;
    uint32_t load = 0;
};
```

Note: `acceptors` moves into `identity`; `actor_types` stays on the entry since it's not part of core identity.

Add `#include <hpactor/adt/node_identity.hpp>` at the top.

- [ ] **Step 5: Build — expect compilation errors from call sites**

```bash
ninja -C build 2>&1 | tail -30
```

Expected: compilation errors in call sites that reference `.endpoint`, `.host`, `.uds_path`, `.acceptors` on Member/NodeEndpoint/PiggybackEntry. This is expected — the next task fixes them.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/adt/node_identity.hpp \
        include/hpactor/net/service_discovery.hpp \
        include/hpactor/net/registrar.hpp \
        include/hpactor/net/gossip_membership.hpp
git commit -m "feat(adt): add NodeIdentity struct, embed in Member/NodeEndpoint/PiggybackEntry"
```

---

### Task 7: Migrate NodeIdentity call sites

**Files:**
- Modify: `src/net/gossip_membership.cpp` — ~45 call sites
- Modify: `src/net/registrar.cpp` — call sites referencing `.endpoint`, `.host`, `.uds_path`, `.acceptors` on `NodeEndpoint`
- Modify: `src/net/hybrid_discovery.cpp` — if it references `.endpoint` on Member
- Modify: `src/net/actor_location_cache.cpp` — if it references `.endpoint` on Member
- Modify: `tests/net/test_gossip_membership.cpp` — call sites referencing `.endpoint` on Member/NodeEndpoint
- Modify: `tests/net/test_service_discovery.cpp` — call sites referencing `.endpoint` on Member

- [ ] **Step 1: Update gossip_membership.cpp call sites**

The migration pattern for every call site: `x.endpoint` → `x.identity.endpoint`, `x.host` → `x.identity.host`, `x.uds_path` → `x.identity.uds_path`, `x.acceptors` → `x.identity.acceptors`.

Key areas in `src/net/gossip_membership.cpp`:

PiggybackEntry serialization (lines ~72, 77, 89, 94, 100):
```cpp
// Before:
ep_to_pb_endpoint(pb->mutable_endpoint(), entry.endpoint);
for (const auto& acc : entry.acceptors) { ... }
entry.endpoint = pb_endpoint_to_ep(pb.endpoint());
entry.acceptors.push_back(acc);

// After:
ep_to_pb_endpoint(pb->mutable_endpoint(), entry.identity.endpoint);
for (const auto& acc : entry.identity.acceptors) { ... }
entry.identity.endpoint = pb_endpoint_to_ep(pb.endpoint());
entry.identity.acceptors.push_back(acc);
```

Member serialization (lines ~108-110, 115, 126-128, 133, 139):
```cpp
// Before:
ep_to_pb_endpoint(pb->mutable_endpoint(), m.endpoint);
pb->set_host(m.host);
pb->set_uds_path(m.uds_path);
for (const auto& acc : m.acceptors) { ... }
m.endpoint = pb_endpoint_to_ep(pb.endpoint());
m.host = pb.host();
m.uds_path = pb.uds_path();
m.acceptors.push_back(acc);

// After:
ep_to_pb_endpoint(pb->mutable_endpoint(), m.identity.endpoint);
pb->set_host(m.identity.host);
pb->set_uds_path(m.identity.uds_path);
for (const auto& acc : m.identity.acceptors) { ... }
m.identity.endpoint = pb_endpoint_to_ep(pb.endpoint());
m.identity.host = pb.host();
m.identity.uds_path = pb.uds_path();
m.identity.acceptors.push_back(acc);
```

local_state references (lines ~242, 383, 393, 402, 410, 418, 420, 421, 424, 436, 531, 579, 583, 593, 599, 623, 642, 685, 963, 968, 1047, 1050, 1054):
```cpp
// Before:
config_.local_state.endpoint
config_.local_state.host
config_.local_state.uds_path
config_.local_state.acceptors

// After:
config_.local_state.identity.endpoint
config_.local_state.identity.host
config_.local_state.identity.uds_path
config_.local_state.identity.acceptors
```

Member access in gossip protocol (lines ~810, 841, 850, 882, 901, 909, 931, 1124, 1127, 1128, 1163, 1164, 1166, 1167, 1177, 1181, 1198, 1201, 1232, 1233):
```cpp
// Before:
m.endpoint = sender;
existing.host = remote.host;
existing.acceptors = remote.acceptors;
members_[entry.endpoint] = ...

// After:
m.identity.endpoint = sender;
existing.identity.host = remote.identity.host;
existing.identity.acceptors = remote.identity.acceptors;
members_[entry.identity.endpoint] = ...
```

Pattern for all: `X.endpoint` → `X.identity.endpoint`, `X.host` → `X.identity.host`, `X.uds_path` → `X.identity.uds_path`, `X.acceptors` → `X.identity.acceptors`. Do a global search-and-replace in this file for each field.

- [ ] **Step 2: Update other src/net/ and test call sites**

Apply the same pattern to all remaining call sites. Search for `.endpoint`, `.host`, `.uds_path`, `.acceptors` on variables of type `Member`, `NodeEndpoint`, or `PiggybackEntry` and prefix with `.identity.`.

```bash
# Find remaining call sites to update
grep -rn '\.endpoint\|\.host\|\.uds_path\|\.acceptors' \
  src/net/registrar.cpp \
  src/net/hybrid_discovery.cpp \
  src/net/actor_location_cache.cpp \
  tests/net/test_gossip_membership.cpp \
  tests/net/test_service_discovery.cpp \
  2>/dev/null | grep -v '\.identity\.'
```

Update each found call site by inserting `.identity` between the variable and the field accessor.

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build 2>&1 | tail -5
ctest --output-on-failure --parallel 8 2>&1 | tail -10
```

Expected: all 140 tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/net/ tests/net/
git commit -m "refactor(adt): migrate call sites to NodeIdentity member access"
```

---

### Task 8: Deduplicate DispatchPolicy enum

**Files:**
- Modify: `include/hpactor/types/types.hpp` — add canonical enum
- Modify: `include/hpactor/sched/dispatch_policy.hpp` — replace with alias
- Modify: `include/hpactor/config/topology_model.hpp:37-41` — remove dup, use alias

- [ ] **Step 1: Add canonical DispatchPolicy to types.hpp**

In `include/hpactor/types/types.hpp`, add after the `Protocol` enum (around line 72):

```cpp
// -----------------------------------------------------------------------------
// DispatchPolicy — how an actor is dispatched to a scheduler worker
// -----------------------------------------------------------------------------
enum class DispatchPolicy : uint8_t {
    Cooperative = 0,
    DedicatedThread,
    DedicatedPool,
};
```

- [ ] **Step 2: Replace sched::DispatchPolicy with alias**

In `include/hpactor/sched/dispatch_policy.hpp`, replace the entire enum definition:

```cpp
#pragma once

#include <hpactor/types/types.hpp>

namespace hpactor::sched {

using DispatchPolicy = hpactor::DispatchPolicy;

struct DispatchHints {
    int cpu_affinity = -1;
    uint32_t pool_size = 1;
    uint8_t priority = 0;
};

} // namespace hpactor::sched
```

- [ ] **Step 3: Replace config::DispatchPolicy with alias**

In `include/hpactor/config/topology_model.hpp`, replace lines 30-41 (the comment and enum definition):

```cpp
// DispatchPolicy is defined in types/types.hpp — config uses the same enum via
// hpactor::DispatchPolicy (no separate config::DispatchPolicy).
using DispatchPolicy = hpactor::DispatchPolicy;
```

Remove the old comment block and `enum class DispatchPolicy : uint8_t { ... };` block.

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build 2>&1 | tail -5
ctest --output-on-failure --parallel 8 2>&1 | tail -10
```

Expected: all 140 tests pass. The `using` alias ensures both `sched::DispatchPolicy` and `config::DispatchPolicy` resolve to `hpactor::DispatchPolicy`.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/types/types.hpp \
        include/hpactor/sched/dispatch_policy.hpp \
        include/hpactor/config/topology_model.hpp
git commit -m "refactor(adt): deduplicate DispatchPolicy enum into types.hpp"
```

---

### Task 9: Create X-macro field definition files

**Files:**
- Create: `include/hpactor/config/system_fields.def`
- Create: `include/hpactor/config/mailbox_fields.def`
- Create: `include/hpactor/config/dispatcher_fields.def`
- Create: `include/hpactor/config/actor_fields.def`

- [ ] **Step 1: Create `include/hpactor/config/system_fields.def`**

Fields shared across Config (runtime), SystemDef (TOML), and BinarySystemDef (mmap). Each line: `HPACTOR_SYSTEM_FIELD(cpp_name, cpp_type, toml_key, default_value)`.

```cpp
// X-macro table: system configuration fields
//
// Columns:
//   1. cpp_name      — field name in Config struct
//   2. cpp_type      — runtime C++ type
//   3. toml_key      — dotted TOML key under [system]
//   4. default_value — default for Config struct initialization
//
// Usage: define HPACTOR_SYSTEM_FIELD before including, then #undef after.
//
//   // Generate struct members:
//   #define HPACTOR_SYSTEM_FIELD(name, type, toml, def) type name{def};
//   #include "system_fields.def"
//   #undef HPACTOR_SYSTEM_FIELD

HPACTOR_SYSTEM_FIELD(scheduler_threads,       size_t,                        "scheduler.threads",            4)
HPACTOR_SYSTEM_FIELD(max_queue_depth,         size_t,                        "scheduler.max_queue_depth",    1024)
HPACTOR_SYSTEM_FIELD(enable_network,          bool,                          "network.enabled",              false)
HPACTOR_SYSTEM_FIELD(tcp_port,                uint16_t,                      "network.tcp_port",             0)
HPACTOR_SYSTEM_FIELD(spawn_timeout_ms,        std::chrono::milliseconds,     "spawn.timeout_ms",             std::chrono::milliseconds{5000})
HPACTOR_SYSTEM_FIELD(enable_http_gateway,     bool,                          "http.enable_gateway",          false)
HPACTOR_SYSTEM_FIELD(http_port,               uint16_t,                      "http.port",                    8080)
HPACTOR_SYSTEM_FIELD(http_bind_host,           std::string,                   "http.bind_host",               "0.0.0.0")
HPACTOR_SYSTEM_FIELD(http_max_connections,    size_t,                        "http.max_connections",         1000)
HPACTOR_SYSTEM_FIELD(http_max_request_size,   size_t,                        "http.max_request_size",        1048576)
HPACTOR_SYSTEM_FIELD(http_reply_timeout_ms,   std::chrono::milliseconds,     "http.reply_timeout_ms",        std::chrono::milliseconds{5000})
```

- [ ] **Step 2: Create `include/hpactor/config/mailbox_fields.def`**

```cpp
// X-macro table: mailbox defaults (shared by Config::MailboxDefaults and TOML SystemMailboxDef)
//
// Usage:
//   #define HPACTOR_MAILBOX_FIELD(name, type, toml, def) type name{def};
//   #include "mailbox_fields.def"
//   #undef HPACTOR_MAILBOX_FIELD

HPACTOR_MAILBOX_FIELD(default_capacity,        uint32_t,                        "capacity",                1024)
HPACTOR_MAILBOX_FIELD(default_byte_capacity,   uint64_t,                        "byte_capacity",           0)
HPACTOR_MAILBOX_FIELD(default_policy,          mailbox::OverflowPolicy,         "overflow_policy",         mailbox::OverflowPolicy::RejectNewest)
HPACTOR_MAILBOX_FIELD(high_watermark,          double,                          "high_watermark",          0.80)
HPACTOR_MAILBOX_FIELD(low_watermark,           double,                          "low_watermark",           0.50)
HPACTOR_MAILBOX_FIELD(protected_system_messages, uint32_t,                      "protected_system_messages", 32)
HPACTOR_MAILBOX_FIELD(backpressure_mode,       mailbox::BackpressureMode,       "backpressure_mode",       mailbox::BackpressureMode::LocalAndRemoteSignal)
```

- [ ] **Step 3: Create `include/hpactor/config/dispatcher_fields.def`**

```cpp
// X-macro table: dispatcher definition fields
//
// Usage:
//   #define HPACTOR_DISPATCHER_FIELD(name, type, def) type name{def};
//   #include "dispatcher_fields.def"
//   #undef HPACTOR_DISPATCHER_FIELD

HPACTOR_DISPATCHER_FIELD(name,          std::string,            "")
HPACTOR_DISPATCHER_FIELD(threads,       uint16_t,               1)
HPACTOR_DISPATCHER_FIELD(cpu_affinity,  std::vector<uint8_t>,   {})
```

- [ ] **Step 4: Create `include/hpactor/config/actor_fields.def`**

```cpp
// X-macro table: actor definition fields
//
// Usage:
//   #define HPACTOR_ACTOR_FIELD(name, type, def) type name{def};
//   #include "actor_fields.def"
//   #undef HPACTOR_ACTOR_FIELD

HPACTOR_ACTOR_FIELD(id,                 std::string,    "")
HPACTOR_ACTOR_FIELD(behavior,           std::string,    "")
HPACTOR_ACTOR_FIELD(supervisor,         std::string,    "")
HPACTOR_ACTOR_FIELD(dispatcher,         std::string,    "")
HPACTOR_ACTOR_FIELD(mailbox_capacity,   uint32_t,       0)
```

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/config/system_fields.def \
        include/hpactor/config/mailbox_fields.def \
        include/hpactor/config/dispatcher_fields.def \
        include/hpactor/config/actor_fields.def
git commit -m "feat(config): add X-macro field definition tables for config schema"
```

---

### Task 10: Update Config and MailboxDefaults to use X-macros

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp:82-162`

- [ ] **Step 1: Replace MailboxDefaults struct with X-macro-generated version**

In `include/hpactor/core/actor_system.hpp`, replace the `MailboxDefaults` struct (lines 82-91):

```cpp
// -----------------------------------------------------------------------------
// MailboxDefaults - system-wide default mailbox configuration
// (generated from config/mailbox_fields.def)
// -----------------------------------------------------------------------------
struct MailboxDefaults {
    #define HPACTOR_MAILBOX_FIELD(name, type, toml, def) type name{def};
    #include <hpactor/config/mailbox_fields.def>
    #undef HPACTOR_MAILBOX_FIELD
};
```

- [ ] **Step 2: Replace Config struct's shared system fields with X-macro**

In `include/hpactor/core/actor_system.hpp`, in the `Config` struct, replace the individual field declarations for the 11 fields defined in `system_fields.def` with the X-macro expansion:

```cpp
struct Config {
    // ── Shared system fields (generated from config/system_fields.def) ──
    #define HPACTOR_SYSTEM_FIELD(name, type, toml, def) type name{def};
    #include <hpactor/config/system_fields.def>
    #undef HPACTOR_SYSTEM_FIELD

    // ── Config-only fields (no TOML/Binary counterpart, or nested structs) ──
    EndPoint endpoint = LocalEndpoint;
    std::chrono::milliseconds spawn_timeout{5000};
    net::TlsConfig tls = {};
    net::PoolConfig pool = {};
    net::RegistrarConfig registrar = {};
    bool enable_http_client = false;
    bool use_coroutines = false;
    cli::CliConfig cli;
    std::shared_ptr<net::IServiceDiscovery> service_discovery = nullptr;
    net::GossipConfig gossip = {};
    MailboxDefaults mailbox;
    mailbox::DeadLetterConfig dead_letters;
    DrainConfig shutdown_drain{DrainPolicy::Drain,
                               std::chrono::milliseconds{30'000}};
    uint32_t ingress_timeout_ms{5000};
    uint32_t cluster_leave_timeout_ms{10000};
    bool shutdown_force_after_timeout{true};
    sched::TimerBackend timer_backend = sched::TimerBackend::TimingWheel;
    bool scheduler_start_paused = false;
    tracing::TraceConfig tracing;
};
```

Note: `spawn_timeout` is renamed to `spawn_timeout_ms` to match the .def field name. Check for any references to `config.spawn_timeout` and update them to `config.spawn_timeout_ms`.

- [ ] **Step 3: Build — expect some compilation errors from spawn_timeout rename**

```bash
ninja -C build 2>&1 | tail -20
```

Search for `.spawn_timeout` references and update to `.spawn_timeout_ms`:

```bash
grep -rn '\.spawn_timeout[^_]' src/ tests/ include/
```

Update each match.

- [ ] **Step 4: Build and fix remaining errors**

```bash
ninja -C build 2>&1 | tail -5
```

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/core/actor_system.hpp
git commit -m "refactor(config): generate Config and MailboxDefaults from X-macro tables"
```

---

### Task 11: Update TOML SystemDef and SystemMailboxDef to use X-macros

**Files:**
- Modify: `include/hpactor/config/topology_model.hpp:86-136`

- [ ] **Step 1: Replace SystemMailboxDef with X-macro**

In `include/hpactor/config/topology_model.hpp`, replace lines 86-98 (`struct SystemMailboxDef { ... };`):

```cpp
// -----------------------------------------------------------------------------
// SystemMailboxDef — system-wide mailbox defaults from [system.mailbox]
// (generated from config/mailbox_fields.def)
// -----------------------------------------------------------------------------
struct SystemMailboxDef {
    #define HPACTOR_MAILBOX_FIELD(name, type, toml, def) type name{def};
    #include <hpactor/config/mailbox_fields.def>
    #undef HPACTOR_MAILBOX_FIELD
};
```

- [ ] **Step 2: Replace SystemDef shared fields with X-macro**

In `include/hpactor/config/topology_model.hpp`, in the `SystemDef` struct, keep the mapping fields but use X-macro for the 11 shared fields. The SystemDef has TOML-specific type variants (e.g., `uint32_t` instead of `size_t` for some fields), so we need a separate X-macro for the TOML types. Add a second X-macro table.

Create `include/hpactor/config/system_toml_fields.def`:

```cpp
// X-macro table: TOML SystemDef fields (TOML types differ from runtime types)
// Columns: cpp_name, toml_type, toml_key, default_value
HPACTOR_SYSTEM_TOML_FIELD(scheduler_threads,       uint32_t,     "scheduler.threads",            4)
HPACTOR_SYSTEM_TOML_FIELD(max_queue_depth,         uint32_t,     "scheduler.max_queue_depth",    1024)
HPACTOR_SYSTEM_TOML_FIELD(enable_network,          bool,         "network.enabled",              false)
HPACTOR_SYSTEM_TOML_FIELD(tcp_port,                uint16_t,     "network.tcp_port",             0)
HPACTOR_SYSTEM_TOML_FIELD(spawn_timeout_ms,        uint32_t,     "spawn.timeout_ms",             5000)
HPACTOR_SYSTEM_TOML_FIELD(enable_http_gateway,     bool,         "http.enable_gateway",          false)
HPACTOR_SYSTEM_TOML_FIELD(http_port,               uint16_t,     "http.port",                    8080)
HPACTOR_SYSTEM_TOML_FIELD(http_bind_host,          std::string,  "http.bind_host",               "0.0.0.0")
HPACTOR_SYSTEM_TOML_FIELD(http_max_connections,    uint32_t,     "http.max_connections",         1000)
HPACTOR_SYSTEM_TOML_FIELD(http_max_request_size,   uint32_t,     "http.max_request_size",        1048576)
HPACTOR_SYSTEM_TOML_FIELD(http_reply_timeout_ms,   uint32_t,     "http.reply_timeout_ms",        5000)
```

Then update SystemDef:

```cpp
struct SystemDef {
    // ── Shared system fields (generated from system_toml_fields.def) ──
    #define HPACTOR_SYSTEM_TOML_FIELD(name, type, toml, def) type name{def};
    #include <hpactor/config/system_toml_fields.def>
    #undef HPACTOR_SYSTEM_TOML_FIELD

    // ── SystemDef-only fields (no Config counterpart, or nested structs) ──
    std::string version;
    uint32_t default_mailbox_size{1024};
    bool metrics_enabled{true};
    uint32_t metrics_ring_buffer_capacity{65536};
    std::string metrics_path{"/metrics"};
    hpactor::log::LogConfig logging;
    hpactor::cli::CliConfig cli;
    std::string default_drain_policy{"Drain"};
    uint32_t default_drain_timeout_ms{30000};
    uint32_t shutdown_ingress_timeout_ms{5000};
    uint32_t shutdown_cluster_leave_timeout_ms{10000};
    bool shutdown_force_after_timeout{true};
    SystemMailboxDef mailbox;
    hpactor::mailbox::DeadLetterConfig dead_letters;
    std::string discovery_backend;
    std::vector<std::string> imports;
    hpactor::tracing::TraceConfig tracing;
};
```

- [ ] **Step 3: Build and verify**

```bash
ninja -C build 2>&1 | tail -5
ctest --output-on-failure --parallel 8 2>&1 | tail -10
```

Expected: all 140 tests pass. The X-macro expansion produces identical struct layouts.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/config/topology_model.hpp \
        include/hpactor/config/system_toml_fields.def
git commit -m "refactor(config): generate SystemDef and SystemMailboxDef from X-macro tables"
```

---

### Task 12: Update TOML parser to use X-macro field conversions

**Files:**
- Modify: `src/config/toml_parser.cpp` — subsystem parser for [system] section

- [ ] **Step 1: Read current parser**

Read `src/config/toml_parser.cpp` to find the `SystemTomlParser` class that parses `[system]` table fields.

- [ ] **Step 2: Add X-macro-generated field copy**

In the parser method that copies `SystemDef` → `Config`, replace the manual field assignments with:

```cpp
// Copy shared fields from SystemDef to Config
#define HPACTOR_SYSTEM_TOML_FIELD(name, type, toml, def) \
    cfg.name = static_cast<decltype(cfg.name)>(def.name);
#include <hpactor/config/system_toml_fields.def>
#undef HPACTOR_SYSTEM_TOML_FIELD

// Copy mailbox defaults
#define HPACTOR_MAILBOX_FIELD(name, type, toml, def) \
    cfg.mailbox.name = def.mailbox.name;
#include <hpactor/config/mailbox_fields.def>
#undef HPACTOR_MAILBOX_FIELD
```

Note: the cast is needed because some types differ (e.g., `uint32_t` → `size_t`).

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build 2>&1 | tail -5
ctest --output-on-failure --parallel 8 2>&1 | tail -10
```

Expected: all 140 tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/config/toml_parser.cpp
git commit -m "refactor(config): use X-macro for SystemDef-to-Config field conversion"
```

---

### Task 13: Update binary serialization to use X-macros

**Files:**
- Modify: `src/config/binary_serializer.cpp`
- Modify: `src/config/binary_loader.cpp`

- [ ] **Step 1: Read binary_serializer.cpp**

Read the current serialization code to understand the manual field writes.

- [ ] **Step 2: Replace manual field writes with X-macro**

For scalar (non-string) fields, use:

```cpp
#define HPACTOR_SYSTEM_FIELD(name, type, toml, def) bin.name = to_binary(cfg.name);
#include <hpactor/config/system_fields.def>
#undef HPACTOR_SYSTEM_FIELD
```

String fields need offset-based writes, handled separately via a helper or a second pass. Since only `http_bind_host` is a string in the shared fields, write it manually or use a conditional macro:

```cpp
// Scalar fields
#define HPACTOR_SYSTEM_SCALAR(name, type, toml, def) bin.name = static_cast<decltype(bin.name)>(cfg.name);
HPACTOR_SYSTEM_SCALAR(scheduler_threads,       size_t,     "scheduler.threads",            4)
HPACTOR_SYSTEM_SCALAR(max_queue_depth,         size_t,     "scheduler.max_queue_depth",    1024)
HPACTOR_SYSTEM_SCALAR(enable_network,          bool,       "network.enabled",              false)
HPACTOR_SYSTEM_SCALAR(tcp_port,                uint16_t,   "network.tcp_port",             0)
HPACTOR_SYSTEM_SCALAR(spawn_timeout_ms,        std::chrono::milliseconds, "spawn.timeout_ms", std::chrono::milliseconds{5000})
HPACTOR_SYSTEM_SCALAR(enable_http_gateway,     bool,       "http.enable_gateway",          false)
HPACTOR_SYSTEM_SCALAR(http_port,               uint16_t,   "http.port",                    8080)
HPACTOR_SYSTEM_SCALAR(http_max_connections,    size_t,     "http.max_connections",         1000)
HPACTOR_SYSTEM_SCALAR(http_max_request_size,   size_t,     "http.max_request_size",        1048576)
HPACTOR_SYSTEM_SCALAR(http_reply_timeout_ms,   std::chrono::milliseconds, "http.reply_timeout_ms", std::chrono::milliseconds{5000})
// String fields (written separately)
bin.http_bind_host_offset = write_string(cfg.http_bind_host);
```

Follow same pattern for `binary_loader.cpp` deserialization.

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build 2>&1 | tail -5
ctest --output-on-failure --parallel 8 2>&1 | tail -10
```

Expected: all 140 tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/config/binary_serializer.cpp src/config/binary_loader.cpp
git commit -m "refactor(config): use X-macro field lists for binary serialization"
```

---

### Task 14: Final build, full test suite, and verification

- [ ] **Step 1: Clean rebuild**

```bash
cd /Users/skg7on/Workspace/Projects/HPActor/.worktrees/shared-adt-extraction
rm -rf build
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tail -5
ninja -C build 2>&1 | tail -5
```

Expected: clean build with zero warnings.

- [ ] **Step 2: Run full test suite**

```bash
ctest --output-on-failure --parallel 8 2>&1
```

Expected: 140/140 tests passing, 0 failures.

- [ ] **Step 3: Run with ASAN**

```bash
cmake -S . -B build-asan -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_ASAN=ON 2>&1 | tail -5
ninja -C build-asan 2>&1 | tail -5
ctest --output-on-failure --parallel 8 --test-dir build-asan 2>&1 | tail -20
```

Expected: tests pass (known ASAN false positives in mailbox_awaiter and priority_scheduler are pre-existing, not regressions).

- [ ] **Step 4: Verify adt/ directory structure**

```bash
ls -la include/hpactor/adt/
```

Expected output:
```
id.hpp
tags.hpp
node_identity.hpp
stream_buffer.hpp
```

- [ ] **Step 5: Commit final state**

```bash
git add -A
git diff --cached --stat
git commit -m "chore: final verification — clean build, 140 tests passing"
```

---

## Summary

| Task | Description | New Files | Modified Files |
|------|-------------|-----------|----------------|
| 1 | Create Id<Tag,T> template | 2 | 0 |
| 2 | Replace ActorId/MessageId/AlarmHandle | 0 | 1 |
| 3 | Replace TimerHandle | 0 | 1 |
| 4 | Replace MessageId::generate() | 0 | 6 |
| 5 | Result type consistency | 0 | 2 |
| 6 | Create NodeIdentity, update structs | 1 | 3 |
| 7 | Migrate NodeIdentity call sites | 0 | ~5 |
| 8 | Deduplicate DispatchPolicy | 0 | 3 |
| 9 | Create X-macro .def files | 4 | 0 |
| 10 | Update Config/MailboxDefaults | 1 | 1 |
| 11 | Update SystemDef/SystemMailboxDef | 1 | 1 |
| 12 | Update TOML parser | 0 | 1 |
| 13 | Update binary serialization | 0 | 2 |
| 14 | Final verification | 0 | 0 |

**Total:** 9 new files, ~25 modified files, 14 commits.
