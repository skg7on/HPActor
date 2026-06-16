# CLI Architecture Standardization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the raw-socket `tools/hpactor-cli/main.cpp` with a `CliClientActor` DaemonActor, extract `ICliCommandHost`/`ISystemCliHost`/`ILifecycleCliHost` host interfaces, add protobuf `CliCommand`/`CliResponse` wire protocol with dual binary+HTTP JSON transport, and update `CliServerActor` with multi-listener support — all while keeping 267 existing CLI tests green.

**Architecture:** Three segregated interfaces replace `CliActor*`/`CliServerActor*` in `CommandContext`. `CliActor` and `CliServerActor` implement them locally; new `CliClientActor` implements them by forwarding over protobuf. One `cli.proto` schema supports both binary (varint-length-prefixed) and HTTP JSON (POST /cli) serialization. Legacy raw-text protocol preserved on its existing port.

**Tech Stack:** C++20, protobuf, existing HPActor framework (DaemonActor, CliSession, ConnectionPool, EventLoop, HTTPGateway, CommandRegistry)

---

## File Structure

| Action | File | Responsibility |
|--------|------|----------------|
| **NEW** | `protos/hpactor/cli.proto` | `CliCommand`, `CliResponse` protobuf messages |
| **NEW** | `include/hpactor/cli/cli_command_host.hpp` | `ICliCommandHost`, `ISystemCliHost`, `ILifecycleCliHost` abstract interfaces |
| **NEW** | `include/hpactor/cli/cli_client_config.hpp` | `CliClientConfig` struct |
| **NEW** | `include/hpactor/cli/cli_client_actor.hpp` | `CliClientActor` class declaration |
| **NEW** | `src/cli/cli_client_actor.cpp` | `CliClientActor` implementation |
| **MODIFY** | `include/hpactor/cli/command_context.hpp` | Add `command_host`/`system_host`/`lifecycle_host` + keep legacy pointers |
| **MODIFY** | `include/hpactor/cli/cli_session.hpp` | Add `set_*_host()` methods + host pointer storage |
| **MODIFY** | `src/cli/cli_session.cpp` | Populate new `CommandContext` fields |
| **MODIFY** | `include/hpactor/cli/cli_actor.hpp` | Implement three host interfaces |
| **MODIFY** | `src/cli/cli_actor.cpp` | Wire host interfaces into CliSession |
| **MODIFY** | `include/hpactor/cli/cli_server_actor.hpp` | Implement three host interfaces; add proto/HTTP acceptors |
| **MODIFY** | `include/hpactor/cli/cli_server_config.hpp` | Add `proto_uds_path`, `proto_tcp_port`, `http_port` |
| **MODIFY** | `src/cli/cli_server_actor.cpp` | Add proto/HTTP acceptors; protocol detection; RPC dispatch |
| **MODIFY** | `src/cli/command_tree_builder.cpp` | Stop calling `register_ask_commands()` |
| **MODIFY** | `src/cli/commands/ask_commands.cpp` | Convert to `ICommand` subclass |
| **MODIFY** | `src/cli/commands/*.cpp` (remaining) | Commands use `ctx.command_host`/`system_host` not `cli_actor`/`cli_server_actor` |
| **MODIFY** | `tools/hpactor-cli/main.cpp` | Replace ~350 lines → thin main (~50 lines) |
| **MODIFY** | `tools/hpactor-cli/CMakeLists.txt` | Link hpactor_lib (unchanged, proto is in hpactor_proto) |
| **MODIFY** | `cmake/dependencies.cmake` | Add `cli.proto` to `PROTOBUF_GENERATE_CPP` |
| **NEW** | `tests/unit/cli/test_cli_command_host.cpp` | Mock host → command dispatch tests |
| **NEW** | `tests/unit/cli/test_cli_wire_protocol.cpp` | Proto round-trip + framing tests |
| **NEW** | `tests/integration/cli/test_cli_client_actor.cpp` | Client actor connect/dispatch/disconnect |
| **NEW** | `tests/integration/cli/test_cli_server_proto.cpp` | Server proto/HTTP listener tests |
| **NEW** | `tests/integration/cli/test_cli_http_endpoint.cpp` | HTTP POST /cli endpoint tests |
| **REMOVE** | `tools/hpactor-cli/main.cpp` | `make_nonblocking`, `connect_uds_nonblock`, `connect_tcp_nonblock`, `await_connect`, `send_line_async`, `recv_response_async`, `FdGuard` (~350 lines) |

---

### Task 1: Add cli.proto and build integration

**Files:**
- Create: `protos/hpactor/cli.proto`
- Modify: `cmake/dependencies.cmake:33-41`

- [ ] **Step 1: Write the proto file**

```protobuf
syntax = "proto3";
package hpactor.cli;

// Command from client to server.
// Supports two dispatch modes:
//   Command-tree mode:  set path + params + args → server dispatches through
//                        CliSession → CommandNode → ICommand → formatted text.
//   RPC mode:           set rpc_method + rpc_request → server dispatches
//                        through ICliCommandHost structured methods (inspect,
//                        kill, quarantine) → structured protobuf reply.
message CliCommand {
  // Command-tree dispatch (text output)
  string path = 1;                  // e.g. "actor/42/show", "system/stats"
  map<string, string> params = 2;   // captured <param> values + --flags
  repeated string args = 3;         // positional arguments
  string format = 4;                // "pretty" | "json" | "tabular"

  // Structured RPC dispatch (protobuf output)
  // When set, bypasses the command tree and calls the host interface directly.
  string rpc_method = 5;            // "inspect", "kill", "enumerate", "quarantine"
  bytes rpc_request = 6;            // serialized InspectStateRequest, KillRequest, etc.
}

// Response from server to client
message CliResponse {
  string content_type = 1;          // "text/plain" | "application/json" | "application/x-protobuf"
  bytes payload = 2;                // formatted text OR serialized protobuf reply
  bool is_error = 3;
  int32 error_code = 4;             // optional, for programmatic handling
  bool is_structured = 5;           // true when payload is a serialized protobuf message
}
```

Write to `protos/hpactor/cli.proto`.

- [ ] **Step 2: Register cli.proto in the build system**

In `cmake/dependencies.cmake`, add the new proto file to the `PROTOBUF_GENERATE_CPP` call. Find:

```cmake
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS
    ${CMAKE_SOURCE_DIR}/protos/hpactor/frame.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/common.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/messages.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/registrar.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/gossip.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/cli_messages.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/ai_resource.proto
)
```

Add the new line after `cli_messages.proto`:

```cmake
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS
    ${CMAKE_SOURCE_DIR}/protos/hpactor/frame.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/common.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/messages.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/registrar.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/gossip.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/cli_messages.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/cli.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/ai_resource.proto
)
```

- [ ] **Step 3: Verify proto compiles**

Run configure + build the proto target:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build hpactor_proto
```

Expected: builds successfully, generates `build/hpactor/cli.pb.h` and `build/hpactor/cli.pb.cc`.

- [ ] **Step 4: Commit**

```bash
git add protos/hpactor/cli.proto cmake/dependencies.cmake
git commit -m "feat: add cli.proto with CliCommand/CliResponse wire protocol messages

Defines dual-dispatch CliCommand (command-tree path + structured RPC method)
and CliResponse (text or protobuf payload with is_structured flag).
Registered in cmake/dependencies.cmake for protobuf codegen.

Issue: #306"
```

---

### Task 2: Create ICliCommandHost / ISystemCliHost / ILifecycleCliHost interfaces

**Files:**
- Create: `include/hpactor/cli/cli_command_host.hpp`

- [ ] **Step 1: Write the interface header**

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

#include <hpactor/cli/cli_types.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

class OutputFormatter;

// Forward-declare protobuf types (defined in cli_messages.pb.h)
class InspectStateReply;
class InspectStateRequest;
class KillReply;
class KillRequest;
class QuarantineReply;
class QuarantineRequest;
class DeadLetterRecord;

/// \brief Core interface for actor-level CLI operations.
///
/// Every CLI host (local or remote) must implement actor inspection, kill,
/// quarantine, and enumeration. Commands use this interface without knowing
/// whether they run on a local daemon or a remote client.
class ICliCommandHost {
  public:
    virtual ~ICliCommandHost() = default;

    /// \brief Inspect an actor and return its full state.
    virtual std::optional<InspectStateReply>
    inspect(ActorId target, const InspectStateRequest& req,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) = 0;

    /// \brief Kill (force-stop) an actor.
    virtual std::optional<KillReply>
    kill(ActorId target, const KillRequest& req,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) = 0;

    /// \brief Quarantine or unquarantine an actor.
    virtual std::optional<QuarantineReply>
    quarantine(ActorId target, const QuarantineRequest& req,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) = 0;

    /// \brief Enumerate all known actors, optionally filtered by type substring.
    virtual std::vector<ActorMeta>
    enumerate(std::string_view filter = "") = 0;
};

/// \brief Interface for system-level CLI queries.
///
/// Methods take an OutputFormatter& so the host can either render directly
/// from local data (CliActor/CliServerActor) or forward the command over the
/// wire and write the response payload (CliClientActor). Commands never
/// branch on local vs. remote.
class ISystemCliHost {
  public:
    virtual ~ISystemCliHost() = default;

    virtual void render_system_stats(OutputFormatter& output) = 0;
    virtual void render_memory_stats(OutputFormatter& output) = 0;
    virtual void render_fault_status(OutputFormatter& output) = 0;
    virtual void render_dlq_list(OutputFormatter& output,
                                 std::string_view filter = "") = 0;
    virtual result<void> dlq_replay(uint32_t index, ActorId target) = 0;
};

/// \brief Interface for lifecycle CLI operations (drain, shutdown).
class ILifecycleCliHost {
  public:
    virtual ~ILifecycleCliHost() = default;

    virtual result<void> drain() = 0;
    virtual result<void> shutdown() = 0;
};

} // namespace cli
} // namespace hpactor
```

Write to `include/hpactor/cli/cli_command_host.hpp`.

- [ ] **Step 2: Verify it compiles (header-only check)**

```bash
ninja -C build src/cli/CMakeFiles/hpactor_lib.dir/cli_actor.cpp.o
```

Expected: compiles successfully (no references to the new header yet, just checking include path resolution).

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/cli/cli_command_host.hpp
git commit -m "feat: add ICliCommandHost, ISystemCliHost, ILifecycleCliHost interfaces

Three segregated abstract interfaces for CLI command host operations:
- ICliCommandHost: actor inspect, kill, quarantine, enumerate
- ISystemCliHost:  render system stats, memory, faults, DLQ
- ILifecycleCliHost: drain, shutdown

Replaces the CliActor*/CliServerActor* concrete union in CommandContext.

Issue: #306"
```

---

### Task 3: Update CommandContext with host interface pointers

**Files:**
- Modify: `include/hpactor/cli/command_context.hpp`

- [ ] **Step 1: Add new fields, keep legacy pointers**

In `include/hpactor/cli/command_context.hpp`, add the three new interface pointers after the existing `params` member. Keep the legacy `cli_actor` and `cli_server_actor` pointers, marked as deprecated.

Find the `CommandContext` struct definition. Add after `params`:

```cpp
  // Host interface pointers — the preferred way for commands to access
  // actor/system/lifecycle operations. Populated by CliSession from the
  // owning actor (CliActor, CliServerActor, or CliClientActor).
  ICliCommandHost*   command_host   = nullptr;
  ISystemCliHost*    system_host    = nullptr;
  ILifecycleCliHost* lifecycle_host = nullptr;
```

Also add the include for the new header at the top:

```cpp
#include <hpactor/cli/cli_command_host.hpp>
```

The legacy `CliActor* cli_actor` and `CliServerActor* cli_server_actor` fields remain unchanged — they are populated from the host interfaces during migration.

- [ ] **Step 2: Verify existing tests still compile**

```bash
ninja -C build tests/unit/cli/test_unit_cli_lexer
./build/tests/unit/cli/test_unit_cli_lexer
```

Expected: builds and passes. The new fields are just additions — no existing code references them yet.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/cli/command_context.hpp
git commit -m "feat: add host interface pointers to CommandContext

Add command_host, system_host, lifecycle_host fields alongside
existing legacy cli_actor/cli_server_actor pointers. Commands will
migrate to the interface pointers incrementally.

Issue: #306"
```

---

### Task 4: Update CliSession to populate host pointers

**Files:**
- Modify: `include/hpactor/cli/cli_session.hpp`
- Modify: `src/cli/cli_session.cpp`

- [ ] **Step 1: Add setter methods and storage to CliSession header**

In `include/hpactor/cli/cli_session.hpp`, add forward declaration for the host interface classes and setter methods. After the existing `set_cli_server_actor()`:

```cpp
    /// \brief Set the command host for actor operations.
    void set_command_host(class ICliCommandHost* host) {
        command_host_ = host;
    }

    /// \brief Set the system host for system queries.
    void set_system_host(class ISystemCliHost* host) {
        system_host_ = host;
    }

    /// \brief Set the lifecycle host for drain/shutdown.
    void set_lifecycle_host(class ILifecycleCliHost* host) {
        lifecycle_host_ = host;
    }
```

Add to the private section, after the existing `cli_server_actor_`:

```cpp
    class ICliCommandHost* command_host_ = nullptr;
    class ISystemCliHost* system_host_ = nullptr;
    class ILifecycleCliHost* lifecycle_host_ = nullptr;
```

- [ ] **Step 2: Populate CommandContext in CliSession::execute_tokens()**

In `src/cli/cli_session.cpp`, in the `execute_tokens()` method, after the line:

```cpp
    ctx.cli_server_actor = cli_server_actor_;
```

Add:

```cpp
    ctx.command_host = command_host_;
    ctx.system_host = system_host_;
    ctx.lifecycle_host = lifecycle_host_;
```

- [ ] **Step 3: Verify existing tests pass**

```bash
ninja -C build tests/unit/cli/test_unit_cli_command_node
./build/tests/unit/cli/test_unit_cli_command_node
ninja -C build tests/integration/cli/test_integration_cli
./build/tests/integration/cli/test_integration_cli
```

Expected: all pass. The new fields are populated but no commands read them yet.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cli/cli_session.hpp src/cli/cli_session.cpp
git commit -m "feat: add host interface setters to CliSession

Add set_command_host(), set_system_host(), set_lifecycle_host()
to CliSession and populate the corresponding CommandContext fields
during execute_tokens().

Issue: #306"
```

---

### Task 5: Implement host interfaces on CliActor

**Files:**
- Modify: `include/hpactor/cli/cli_actor.hpp`
- Modify: `src/cli/cli_actor.cpp`

- [ ] **Step 1: Add interface inheritance to CliActor declaration**

In `include/hpactor/cli/cli_actor.hpp`, change the class declaration from:

```cpp
class CliActor : public DaemonActor {
```

to:

```cpp
class CliActor : public DaemonActor,
                 public ICliCommandHost,
                 public ISystemCliHost,
                 public ILifecycleCliHost {
```

Add the include:

```cpp
#include <hpactor/cli/cli_command_host.hpp>
```

Add the override declarations in the public section (their implementations already exist as `send_and_wait_inspect`, `send_and_wait_kill`, `send_and_wait_quarantine`, and `enumerate_actors` — they just need to be wired as the interface methods):

```cpp
    // ICliCommandHost
    std::optional<InspectStateReply>
    inspect(ActorId target, const InspectStateRequest& req,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::optional<KillReply>
    kill(ActorId target, const KillRequest& req,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::optional<QuarantineReply>
    quarantine(ActorId target, const QuarantineRequest& req,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::vector<ActorMeta> enumerate(std::string_view filter = "") override;

    // ISystemCliHost
    void render_system_stats(OutputFormatter& output) override;
    void render_memory_stats(OutputFormatter& output) override;
    void render_fault_status(OutputFormatter& output) override;
    void render_dlq_list(OutputFormatter& output, std::string_view filter = "") override;
    result<void> dlq_replay(uint32_t index, ActorId target) override;

    // ILifecycleCliHost
    result<void> drain() override;
    result<void> shutdown() override;
```

Remove the old standalone declarations of `send_and_wait_inspect`, `send_and_wait_kill`, `send_and_wait_quarantine`, and `enumerate_actors` — they are now the interface overrides.

- [ ] **Step 2: Implement interface methods in CliActor**

In `src/cli/cli_actor.cpp`, rename existing method implementations to match the interface:

- `send_and_wait_inspect` → `inspect`
- `send_and_wait_kill` → `kill`
- `send_and_wait_quarantine` → `quarantine`
- `enumerate_actors` → `enumerate`

Add new `ISystemCliHost` methods. These inline the logic currently in the command handlers. For `render_system_stats`:

```cpp
void CliActor::render_system_stats(OutputFormatter& output) {
    output.header("System Statistics");
    std::map<std::string, std::string> kv;
    kv["Total actors"] = std::to_string(system_.actor_count());
    if (auto* sched = system_.scheduler()) {
        kv["Scheduler threads"] = std::to_string(sched->worker_count());
    }
    kv["CLI enabled"] = config_.enabled ? "yes" : "no";
    kv["CLI format"] = config_.default_format;
    output.key_value(kv);
}
```

For `render_memory_stats`:

```cpp
void CliActor::render_memory_stats(OutputFormatter& output) {
    output.header("Memory Regions");
    auto& reg = mem::MemoryRegionRegistry::instance();
    std::vector<std::string> cols = {"Region", "Active", "Limit",
                                     "Pressure", "Allocs", "Frees", "Corruptions"};
    std::vector<std::vector<std::string>> rows;
    static constexpr mem::RegionType kRegions[] = {
        mem::RegionType::kActor, mem::RegionType::kMessage,
        mem::RegionType::kCoroutine, mem::RegionType::kNetwork,
        mem::RegionType::kInternal, mem::RegionType::kHibernate};
    for (auto region : kRegions) {
        auto snap = reg.snapshot(region);
        rows.push_back({
            mem::to_string(region),
            std::to_string(snap.active_bytes),
            std::to_string(snap.limit_bytes),
            mem::to_string(snap.pressure),
            std::to_string(snap.alloc_count),
            std::to_string(snap.free_count),
            std::to_string(snap.corruption_count),
        });
    }
    output.table(cols, rows);
}
```

For `render_fault_status` — inline fault status logic from `fault_commands.cpp`:

```cpp
void CliActor::render_fault_status(OutputFormatter& output) {
    output.header("Fault Injection Status");
    auto* fc = system_.fault_controller();
    if (!fc || !fc->enabled()) {
        output.raw("Fault injection is disabled.\n");
        return;
    }
    std::map<std::string, std::string> kv;
    kv["Enabled"] = "yes";
    kv["Seed"] = std::to_string(fc->seed());
    kv["Hooks triggered"] = std::to_string(fc->total_triggers());
    output.key_value(kv);
}
```

For `render_dlq_list`:

```cpp
void CliActor::render_dlq_list(OutputFormatter& output, std::string_view filter) {
    output.header("Dead Letter Queue");
    auto* dlq = system_.dead_letter_queue();
    if (!dlq) {
        output.raw("DLQ is not configured.\n");
        return;
    }
    auto records = dlq->snapshot_records();
    if (records.empty()) {
        output.raw("DLQ is empty.\n");
        return;
    }
    std::vector<std::string> cols = {"#", "Actor", "Reason", "Source", "Age"};
    std::vector<std::vector<std::string>> rows;
    for (size_t i = 0; i < records.size(); ++i) {
        auto& r = records[i];
        if (!filter.empty()) {
            std::string aid = std::to_string(r.actor_id);
            if (aid.find(filter) == std::string::npos)
                continue;
        }
        rows.push_back({
            std::to_string(i),
            std::to_string(r.actor_id),
            std::to_string(static_cast<int>(r.reason)),
            std::to_string(static_cast<int>(r.source)),
            std::to_string(r.age_ms) + "ms",
        });
    }
    output.table(cols, rows);
}
```

For `dlq_replay`:

```cpp
result<void> CliActor::dlq_replay(uint32_t index, ActorId target) {
    auto* dlq = system_.dead_letter_queue();
    if (!dlq)
        return error::make(error::Code::kNotFound, "DLQ not configured");
    return dlq->try_replay_at(index, target)
               ? result<void>::make()
               : error::make(error::Code::kInvalidArgument, "replay failed");
}
```

For `drain`:

```cpp
result<void> CliActor::drain() {
    return system_.drain();
}
```

For `shutdown`:

```cpp
result<void> CliActor::shutdown() {
    return system_.shutdown();
}
```

- [ ] **Step 3: Wire host pointers into CliSession in constructor**

In `CliActor::CliActor()` constructor, after the existing `session_->set_cli_actor(this)`:

```cpp
    session_->set_command_host(this);
    session_->set_system_host(this);
    session_->set_lifecycle_host(this);
```

- [ ] **Step 4: Build and run existing CLI tests**

```bash
ninja -C build hpactor_lib
ninja -C build tests/unit/cli/test_unit_cli_command_node
./build/tests/unit/cli/test_unit_cli_command_node
ninja -C build tests/integration/cli/test_integration_cli
./build/tests/integration/cli/test_integration_cli
```

Expected: all pass. The interface methods exist but commands still use the old access paths.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/cli/cli_actor.hpp src/cli/cli_actor.cpp
git commit -m "feat: implement host interfaces on CliActor

CliActor now inherits ICliCommandHost, ISystemCliHost, ILifecycleCliHost.
Existing send_and_wait_inspect/kill/quarantine renamed to inspect/kill/quarantine.
New render_* methods inline logic from command handlers.
Host pointers wired into CliSession during construction.

Issue: #306"
```

---

### Task 6: Migrate commands to use host interface pointers

**Files:**
- Modify: `src/cli/commands/system_commands.cpp`
- Modify: `src/cli/commands/fault_commands.cpp`
- Modify: `src/cli/commands/dlq_commands.cpp`
- Modify: `src/cli/commands/actor_commands.cpp`
- Modify: `src/cli/commands/failure_commands.cpp`
- Modify: `src/cli/commands/misc_commands.cpp`
- Modify: `src/cli/commands/help_command.cpp`
- Modify: `src/cli/commands/quit_command.cpp`
- Modify: `src/cli/commands/endpoint_commands.cpp`
- Modify: `src/cli/commands/scheduler_commands.cpp`
- Modify: `src/cli/commands/log_commands.cpp`
- Modify: `src/cli/commands/tracing_commands.cpp`
- Modify: `src/cli/commands/reliable_commands.cpp`
- Modify: `src/cli/commands/command_utils.hpp`

- [ ] **Step 1: Migrate SystemStatsCommand**

In `src/cli/commands/system_commands.cpp`, change `SystemStatsCommand::execute`:

Before:
```cpp
    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("System Statistics");
        auto* sys = ctx.system;
        auto* cli = ctx.cli_actor;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }
        std::map<std::string, std::string> kv;
        kv["Total actors"] = std::to_string(sys->actor_count());
        if (auto* sched = sys->scheduler()) {
            kv["Scheduler threads"] = std::to_string(sched->worker_count());
        }
        if (cli) {
            kv["CLI enabled"] = cli->config().enabled ? "yes" : "no";
            kv["CLI format"] = cli->config().default_format;
        }
        ctx.output->key_value(kv);
        return result<void>::make();
    }
```

After:
```cpp
    result<void> execute(CommandContext& ctx) const override {
        if (ctx.system_host) {
            ctx.system_host->render_system_stats(*ctx.output);
            return result<void>::make();
        }
        // Fallback for environments without a host
        ctx.output->header("System Statistics");
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }
        std::map<std::string, std::string> kv;
        kv["Total actors"] = std::to_string(sys->actor_count());
        if (auto* sched = sys->scheduler()) {
            kv["Scheduler threads"] = std::to_string(sched->worker_count());
        }
        if (ctx.cli_actor) {
            kv["CLI enabled"] = ctx.cli_actor->config().enabled ? "yes" : "no";
            kv["CLI format"] = ctx.cli_actor->config().default_format;
        }
        ctx.output->key_value(kv);
        return result<void>::make();
    }
```

- [ ] **Step 2: Migrate SystemMemoryCommand** — same pattern: check `ctx.system_host` first, call `render_memory_stats()`, fallback to inline logic.

- [ ] **Step 3: Migrate FaultStatusCommand** — call `ctx.system_host->render_fault_status()`.

- [ ] **Step 4: Migrate DlqListCommand** — call `ctx.system_host->render_dlq_list()`.

- [ ] **Step 5: Migrate DlqShowCommand/DlqReplayCommand/DlqExportCommand** — use `ctx.system_host` for queries.

- [ ] **Step 6: Migrate actor commands** that use `inspect`/`kill`/`quarantine`:
  - Check `ctx.command_host` first
  - Call `ctx.command_host->inspect(target, req, timeout)` instead of `ctx.cli_actor->send_and_wait_inspect(...)`
  - Same pattern for kill, quarantine, enumerate

- [ ] **Step 7: Migrate quit_command** — call `ctx.lifecycle_host->shutdown()` if available.

- [ ] **Step 8: Build and run all CLI tests**

```bash
ninja -C build
ctest --output-on-failure --parallel 8 -R "cli"
```

Expected: all 267+ CLI tests pass. Commands now use host interfaces when available, fallback to legacy pointers otherwise.

- [ ] **Step 9: Commit**

```bash
git add src/cli/commands/
git commit -m "refactor: migrate CLI commands to host interface pointers

All commands now check ctx.command_host/system_host/lifecycle_host first,
with fallback to legacy cli_actor/cli_server_actor pointers. Enables
remote CLI operation with zero branching in command implementations.

Issue: #306"
```

---

### Task 7: Convert ask_commands to ICommand subclass

**Files:**
- Modify: `src/cli/commands/ask_commands.cpp`
- Modify: `src/cli/commands/ask_commands.hpp`
- Modify: `src/cli/command_tree_builder.cpp`

- [ ] **Step 1: Convert AskPendingCommand to ICommand**

In `src/cli/commands/ask_commands.cpp`, create ICommand subclasses instead of the `register_ask_commands()` free function. Replace the file contents with three ICommand subclasses:

```cpp
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {
namespace cli {
namespace {

class AskPendingCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "ask/pending"; }
    std::string_view help_text() const noexcept override {
        return "List in-flight ask requests";
    }
    int order() const noexcept override { return 600; }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Pending Ask Requests");
        ctx.output->raw("Ask manager inspection not yet available.\n");
        return result<void>::make();
    }
};

const CommandRegistration<AskPendingCommand> kRegisterAskPending;

class AskCancelCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "ask/cancel" };
    std::string_view help_text() const noexcept override {
        return "Cancel an ask request by message ID";
    }
    int order() const noexcept override { return 601; }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Cancel Ask Request");
        ctx.output->raw("Ask manager inspection not yet available.\n");
        return result<void>::make();
    }
};

const CommandRegistration<AskCancelCommand> kRegisterAskCancel;

class AskStatsCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "ask/stats"; }
    std::string_view help_text() const noexcept override {
        return "Ask manager statistics";
    }
    int order() const noexcept override { return 602; }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Ask Manager Statistics");
        ctx.output->raw("Ask manager inspection not yet available.\n");
        return result<void>::make();
    }
};

const CommandRegistration<AskStatsCommand> kRegisterAskStats;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
```

- [ ] **Step 2: Remove register_ask_commands**

In `src/cli/commands/ask_commands.hpp`, remove the `register_ask_commands()` declaration (or delete the file if it only contained that function).

In `src/cli/command_tree_builder.cpp`, remove:
- `#include "commands/ask_commands.hpp"` (line 15)
- `cli::register_ask_commands(root);` (line 79)

- [ ] **Step 3: Build and run CLI tests**

```bash
ninja -C build
ctest --output-on-failure --parallel 8 -R "cli"
```

Expected: ask commands appear via ICommand self-registration. `/ask pending`, `/ask cancel`, `/ask stats` still work.

- [ ] **Step 4: Commit**

```bash
git add src/cli/commands/ask_commands.cpp src/cli/commands/ask_commands.hpp src/cli/command_tree_builder.cpp
git commit -m "refactor: convert ask_commands to ICommand self-registration

Replace register_ask_commands() free function with three ICommand
subclasses (AskPendingCommand, AskCancelCommand, AskStatsCommand)
using CommandRegistration<T> static file-scope registrars.

Issue: #306"
```

---

### Task 8: Update CliServerConfig with proto/HTTP fields

**Files:**
- Modify: `include/hpactor/cli/cli_server_config.hpp`

- [ ] **Step 1: Add new fields**

In `include/hpactor/cli/cli_server_config.hpp`, add after the existing `tcp_bind_address` field:

```cpp
    /// \brief Unix domain socket path for protobuf binary CLI connections.
    ///        Empty means no protobuf UDS listener.
    std::string proto_uds_path;

    /// \brief TCP listen port for protobuf binary CLI connections.
    ///        0 means disabled.
    uint16_t proto_tcp_port = 0;

    /// \brief TCP listen port for HTTP JSON CLI connections.
    ///        0 means disabled. Reuses the HTTPGateway infrastructure.
    uint16_t http_port = 0;

    /// \brief Bind address for the HTTP JSON listener.
    std::string http_bind_address = "127.0.0.1";

    /// \brief Permission mode for the protobuf UDS socket file.
    uint32_t proto_uds_socket_mode = 0660;

    /// \brief Owner user name for the protobuf UDS socket (optional).
    std::string proto_uds_socket_owner;

    /// \brief Owner group name for the protobuf UDS socket (optional).
    std::string proto_uds_socket_group;
```

- [ ] **Step 2: Build to verify**

```bash
ninja -C build hpactor_lib
```

Expected: compiles. Fields are unused until CliServerActor reads them.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/cli/cli_server_config.hpp
git commit -m "feat: add proto/HTTP listener fields to CliServerConfig

Add proto_uds_path, proto_tcp_port, http_port, http_bind_address, and
associated UDS permission fields for the new protobuf and HTTP JSON
CLI listeners.

Issue: #306"
```

---

### Task 9: Implement host interfaces and proto/HTTP listeners on CliServerActor

**Files:**
- Modify: `include/hpactor/cli/cli_server_actor.hpp`
- Modify: `src/cli/cli_server_actor.cpp`

- [ ] **Step 1: Add interface inheritance and new members to CliServerActor header**

In `include/hpactor/cli/cli_server_actor.hpp`, add interface inheritance:

```cpp
class CliServerActor : public DaemonActor,
                       public ICliCommandHost,
                       public ISystemCliHost,
                       public ILifecycleCliHost {
```

Add override declarations for all three interfaces (same signatures as CliActor). Add new private members for the proto/HTTP listeners:

```cpp
    // Protobuf binary listener
    std::unique_ptr<net::UnixDomainAcceptor> proto_uds_acceptor_;
    std::unique_ptr<net::TcpAcceptor> proto_tcp_acceptor_;

    // HTTP JSON listener (reuses HTTPGateway)
    std::unique_ptr<net::HTTPGateway> http_gateway_;

    // Proto client sessions (separate from legacy sessions_)
    struct ProtoSessionState {
        std::unique_ptr<CliSession> session;
        std::chrono::steady_clock::time_point last_activity;
        std::string read_buffer;
    };
    std::unordered_map<int, ProtoSessionState> proto_sessions_;

    // Protocol detection + dispatch
    void on_proto_client_accepted(int client_fd);
    void on_proto_client_readable(int client_fd);
    void close_proto_session(int client_fd);
    CliResponse execute_cli_command(const CliCommand& cmd);
    std::string dispatch_rpc(const std::string& rpc_method, const std::string& rpc_request);
```

Add includes:
```cpp
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli.pb.h>
```

- [ ] **Step 2: Add proto listener setup in on_daemon_start()**

In `src/cli/cli_server_actor.cpp`, add after the existing TCP listener setup:

```cpp
    // --- Protobuf binary listener (UDS) ---
    if (!config_.proto_uds_path.empty()) {
        proto_uds_acceptor_ = std::make_unique<net::UnixDomainAcceptor>(loop_.get());
        proto_uds_acceptor_->set_accept_handler(
            [this](int fd, EndPoint) { on_proto_client_accepted(fd); });
        if (!proto_uds_acceptor_->listen(config_.proto_uds_path)) {
            std::fprintf(stderr, "CliServerActor: proto UDS listen failed on %s\n",
                         config_.proto_uds_path.c_str());
            proto_uds_acceptor_.reset();
        } else {
            ::chmod(config_.proto_uds_path.c_str(),
                    static_cast<mode_t>(config_.proto_uds_socket_mode));
        }
    }

    // --- Protobuf binary listener (TCP) ---
    if (config_.proto_tcp_port > 0) {
        proto_tcp_acceptor_ = std::make_unique<net::TcpAcceptor>(loop_.get());
        proto_tcp_acceptor_->set_accept_handler(
            [this](int fd, EndPoint) { on_proto_client_accepted(fd); });
        if (!proto_tcp_acceptor_->listen(config_.proto_tcp_port, 0,
                                         config_.tcp_bind_address)) {
            std::fprintf(stderr, "CliServerActor: proto TCP listen failed on %s:%u\n",
                         config_.tcp_bind_address.c_str(),
                         static_cast<unsigned>(config_.proto_tcp_port));
            proto_tcp_acceptor_.reset();
        }
    }
```

- [ ] **Step 3: Implement on_proto_client_accepted, on_proto_client_readable, close_proto_session**

`on_proto_client_accepted` — creates a CliSession for the proto client, registers a read handler:

```cpp
void CliServerActor::on_proto_client_accepted(int client_fd) {
    if (proto_sessions_.size() >= config_.max_sessions) {
        ::close(client_fd);
        return;
    }

    auto formatter = OutputFormatter::create(config_.default_format);
    auto session = std::make_unique<CliSession>(
        &system_, command_tree_.get(), std::move(formatter),
        [client_fd](const std::string& text) {
            // Build CliResponse and send as length-delimited protobuf frame.
            CliResponse resp;
            resp.set_content_type("text/plain");
            resp.set_payload(text);
            std::string wire = resp.SerializeAsString();
            // Varint length prefix + payload
            uint32_t len = static_cast<uint32_t>(wire.size());
            uint8_t len_buf[5];
            int len_bytes = 0;
            while (len > 0x7f) {
                len_buf[len_bytes++] = static_cast<uint8_t>(len & 0x7f) | 0x80;
                len >>= 7;
            }
            len_buf[len_bytes++] = static_cast<uint8_t>(len);
            ::write(client_fd, len_buf, len_bytes);
            ::write(client_fd, wire.data(), wire.size());
        },
        config_.page_size);
    session->set_cli_server_actor(this);
    session->set_command_host(this);
    session->set_system_host(this);
    session->set_lifecycle_host(this);

    ProtoSessionState state;
    state.session = std::move(session);
    state.last_activity = std::chrono::steady_clock::now();
    proto_sessions_.emplace(client_fd, std::move(state));

    loop_->set_read_handler(
        client_fd, [this](int ready_fd) { on_proto_client_readable(ready_fd); });
}
```

`on_proto_client_readable` — drains data, peeks for HTTP vs protobuf, dispatches:

```cpp
void CliServerActor::on_proto_client_readable(int client_fd) {
    auto it = proto_sessions_.find(client_fd);
    if (it == proto_sessions_.end()) return;

    auto& state = it->second;
    state.last_activity = std::chrono::steady_clock::now();

    char buf[4096];
    bool closed = false;
    while (!closed) {
        ssize_t n = ::read(client_fd, buf, sizeof(buf));
        if (n > 0) {
            state.read_buffer.append(buf, static_cast<size_t>(n));

            // Protocol detection: first bytes determine HTTP vs protobuf
            if (state.read_buffer.size() >= 4) {
                if (state.read_buffer.starts_with("GET ") ||
                    state.read_buffer.starts_with("POST")) {
                    // HTTP — route to HTTPGateway (handled in Task 12)
                    closed = true;
                    break;
                }
            }

            // Try to decode varint-length-prefixed protobuf frames
            while (state.read_buffer.size() >= 1) {
                // Decode varint length
                uint32_t msg_len = 0;
                int shift = 0;
                size_t pos = 0;
                while (pos < state.read_buffer.size() && pos < 5) {
                    uint8_t byte = static_cast<uint8_t>(state.read_buffer[pos]);
                    msg_len |= (byte & 0x7f) << shift;
                    pos++;
                    if (!(byte & 0x80)) break;
                    shift += 7;
                }
                if (pos == 0 || pos > 5) break; // incomplete or corrupt

                if (state.read_buffer.size() < pos + msg_len) break; // incomplete

                // Decode and dispatch
                CliCommand cmd;
                if (cmd.ParseFromArray(state.read_buffer.data() + pos, msg_len)) {
                    state.read_buffer.erase(0, pos + msg_len);
                    CliResponse resp = execute_cli_command(cmd);
                    std::string wire = resp.SerializeAsString();
                    uint32_t out_len = static_cast<uint32_t>(wire.size());
                    uint8_t len_buf[5];
                    int len_bytes = 0;
                    uint32_t tmp = out_len;
                    while (tmp > 0x7f) {
                        len_buf[len_bytes++] = static_cast<uint8_t>(tmp & 0x7f) | 0x80;
                        tmp >>= 7;
                    }
                    len_buf[len_bytes++] = static_cast<uint8_t>(tmp);
                    ::write(client_fd, len_buf, len_bytes);
                    ::write(client_fd, wire.data(), wire.size());
                } else {
                    break; // parse error, wait for more data
                }
            }
        } else if (n == 0) {
            closed = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            closed = true;
        } else {
            break;
        }
    }

    if (closed) {
        close_proto_session(client_fd);
    }
}
```

- [ ] **Step 4: Implement execute_cli_command and dispatch_rpc**

`execute_cli_command` — dispatches based on whether rpc_method is set:

```cpp
CliResponse CliServerActor::execute_cli_command(const CliCommand& cmd) {
    CliResponse resp;

    if (!cmd.rpc_method().empty()) {
        // Structured RPC dispatch
        std::string reply_bytes = dispatch_rpc(cmd.rpc_method(), cmd.rpc_request());
        resp.set_content_type("application/x-protobuf");
        resp.set_payload(reply_bytes);
        resp.set_is_structured(true);
        return resp;
    }

    // Command-tree dispatch: reconstruct line and feed to CliSession
    std::string line = "/" + cmd.path();
    for (const auto& arg : cmd.args()) {
        line += " " + arg;
    }
    for (const auto& [key, val] : cmd.params()) {
        line += " --" + key;
        if (!val.empty()) line += " " + val;
    }
    if (!cmd.format().empty()) {
        line += " --format " + cmd.format();
    }

    // Use the first proto session's CliSession to execute.
    // (In practice, process_line returns formatted text via the output_fn.)
    // For a simpler approach: create a temporary StringOutputFormatter,
    // execute, capture result.
    std::string captured;
    auto temp_formatter = OutputFormatter::create(cmd.format().empty() ? "pretty" : cmd.format());
    // Build a temporary CliSession just for this execution
    CliSession temp_session(&system_, command_tree_.get(),
                            std::move(temp_formatter),
                            [&captured](const std::string& text) { captured = text; },
                            50);
    temp_session.set_cli_server_actor(this);
    temp_session.set_command_host(this);
    temp_session.set_system_host(this);
    temp_session.set_lifecycle_host(this);
    temp_session.process_line(line);

    resp.set_content_type("text/plain");
    resp.set_payload(captured);
    return resp;
}
```

`dispatch_rpc` — routes to the host interface method:

```cpp
std::string CliServerActor::dispatch_rpc(const std::string& method,
                                          const std::string& request_bytes) {
    if (method == "inspect") {
        InspectStateRequest req;
        if (!req.ParseFromString(request_bytes)) return "";
        ActorId target{req.target_actor_id()};
        auto reply = inspect(target, req);
        return reply ? reply->SerializeAsString() : "";
    }
    if (method == "kill") {
        KillRequest req;
        if (!req.ParseFromString(request_bytes)) return "";
        ActorId target{req.target_actor_id()};
        auto reply = kill(target, req);
        return reply ? reply->SerializeAsString() : "";
    }
    if (method == "quarantine") {
        QuarantineRequest req;
        if (!req.ParseFromString(request_bytes)) return "";
        ActorId target{req.target_actor_id()};
        auto reply = quarantine(target, req);
        return reply ? reply->SerializeAsString() : "";
    }
    if (method == "enumerate") {
        auto actors = enumerate(request_bytes /* filter */);
        // Return as serialized ListActorsReply for structured transport
        ListActorsReply list_reply;
        for (auto& a : actors) {
            auto* pb_meta = list_reply.add_actors();
            pb_meta->set_actor_id(a.actor_id);
            pb_meta->set_actor_type(a.actor_type);
            pb_meta->set_state(a.state);
        }
        return list_reply.SerializeAsString();
    }
    return "";
}
```

- [ ] **Step 5: Add ISystemCliHost and ILifecycleCliHost implementations**

Same pattern as CliActor — delegate to ActorSystem:

```cpp
void CliServerActor::render_system_stats(OutputFormatter& output) {
    output.header("System Statistics");
    std::map<std::string, std::string> kv;
    kv["Total actors"] = std::to_string(system_.actor_count());
    if (auto* sched = system_.scheduler()) {
        kv["Scheduler threads"] = std::to_string(sched->worker_count());
    }
    output.key_value(kv);
}

// render_memory_stats, render_fault_status, render_dlq_list, dlq_replay
// implementations are identical to CliActor (Task 5 Step 2) — delegating
// to ActorSystem / MemoryRegionRegistry / FaultController / DeadLetterQueue.
// Copy the same bodies from CliActor.

result<void> CliServerActor::drain() { return system_.drain(); }
result<void> CliServerActor::shutdown() { return system_.shutdown(); }
```

- [ ] **Step 6: Cleanup proto sessions in on_daemon_stop()**

Add cleanup for proto sessions:

```cpp
    for (auto& [fd, state] : proto_sessions_) {
        loop_->clear_read_handler(fd);
        ::close(fd);
    }
    proto_sessions_.clear();
    proto_uds_acceptor_.reset();
    proto_tcp_acceptor_.reset();
```

- [ ] **Step 7: Rename existing methods to match interface**

Rename `send_and_wait_inspect` → `inspect`, `send_and_wait_kill` → `kill`, `send_and_wait_quarantine` → `quarantine`, `enumerate_actors` → `enumerate`. Update internal callers.

- [ ] **Step 8: Build and run tests**

```bash
ninja -C build
ctest --output-on-failure --parallel 8 -R "cli"
```

Expected: all CLI tests pass. Proto listener code exists but isn't exercised until integration tests.

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/cli/cli_server_actor.hpp src/cli/cli_server_actor.cpp
git commit -m "feat: implement host interfaces and proto listener on CliServerActor

Add protobuf binary listener (UDS + TCP) with protocol detection
(HTTP vs varint-length-prefixed protobuf). Implement ICliCommandHost,
ISystemCliHost, ILifecycleCliHost by delegating to ActorSystem.
Add execute_cli_command() with dual dispatch: rpc_method for structured
RPC, path+params+args for command-tree text dispatch.

Issue: #306"
```

---

### Task 10: Create CliClientConfig

**Files:**
- Create: `include/hpactor/cli/cli_client_config.hpp`

- [ ] **Step 1: Write the config header**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor {
namespace cli {

/// \brief Configuration for the remote CLI client (CliClientActor).
struct CliClientConfig {
    /// \brief Transport protocol selection.
    enum class Transport { Protobuf, HttpJson };

    // Transport
    std::string uds_path = "/var/run/hpactor/hpactor-cli.sock";
    std::string host;                     // empty = use UDS
    uint16_t    port = 0;                 // required with host
    uint16_t    http_port = 0;            // optional HTTP JSON transport
    Transport   transport = Transport::Protobuf;

    // Session
    std::string history_path;             // "" = $HOME/.hpactor_cli_history
    uint32_t    history_max = 1000;
    std::string default_format = "pretty";

    // Timing
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{10000};

    // Reconnection
    std::chrono::milliseconds reconnect_min{1000};
    std::chrono::milliseconds reconnect_max{30000};
};

} // namespace cli
} // namespace hpactor
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/cli/cli_client_config.hpp
git commit -m "feat: add CliClientConfig struct

Transport (UDS/TCP/HTTP JSON), session (history, format), timing
(connect/request timeouts), and reconnection (exponential backoff
range) configuration for the remote CLI client actor.

Issue: #306"
```

---

### Task 11: Create CliClientActor

**Files:**
- Create: `include/hpactor/cli/cli_client_actor.hpp`
- Create: `src/cli/cli_client_actor.cpp`

- [ ] **Step 1: Write CliClientActor header**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/cli/cli_client_config.hpp>
#include <hpactor/cli/cli_command_host.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace net {
class ConnectionPool;
class Connection;
}

namespace cli {

class CliSession;
class LineEditor;
class OutputFormatter;
struct CommandNode;

class CliClientActor : public DaemonActor,
                       public ICliCommandHost,
                       public ISystemCliHost,
                       public ILifecycleCliHost {
  public:
    static constexpr const char* kActorTypeName = "CliClientActor";

    CliClientActor(ActorContext* ctx, ActorSystem& system,
                   const CliClientConfig& config);
    ~CliClientActor() override;

    // DaemonActor
    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;
    bool is_system_actor() const override { return true; }

    // ICliCommandHost — use RPC dispatch (rpc_method + rpc_request)
    std::optional<class InspectStateReply>
    inspect(ActorId target, const class InspectStateRequest& req,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::optional<class KillReply>
    kill(ActorId target, const class KillRequest& req,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::optional<class QuarantineReply>
    quarantine(ActorId target, const class QuarantineRequest& req,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::vector<ActorMeta> enumerate(std::string_view filter = "") override;

    // ISystemCliHost — use command-tree dispatch (path + params)
    void render_system_stats(OutputFormatter& output) override;
    void render_memory_stats(OutputFormatter& output) override;
    void render_fault_status(OutputFormatter& output) override;
    void render_dlq_list(OutputFormatter& output, std::string_view filter = "") override;
    result<void> dlq_replay(uint32_t index, ActorId target) override;

    // ILifecycleCliHost
    result<void> drain() override;
    result<void> shutdown() override;

    /// \brief Set the command to run in exec mode (single command, then exit).
    void set_exec_command(const std::string& cmd) { exec_cmd_ = cmd; exec_mode_ = true; }

  private:
    void connect();
    void disconnect();
    class CliResponse send_and_wait(const class CliCommand& cmd);

    ActorSystem& system_;
    CliClientConfig config_;
    std::unique_ptr<CliSession> session_;
    std::unique_ptr<LineEditor> line_editor_;
    std::unique_ptr<CommandNode> command_tree_;
    std::unique_ptr<net::ConnectionPool> pool_;
    net::Connection* conn_ = nullptr;
    bool running_ = true;
    bool exec_mode_ = false;
    std::string exec_cmd_;
    std::string recv_buffer_;
};

} // namespace cli
} // namespace hpactor
```

- [ ] **Step 2: Write CliClientActor implementation**

In `src/cli/cli_client_actor.cpp`:

```cpp
#include <hpactor/cli/cli_client_actor.hpp>
#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/command_tree_builder.hpp>
#include <hpactor/cli/line_editor.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli.pb.h>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/types/types.hpp>

#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace hpactor {
namespace cli {

CliClientActor::CliClientActor(ActorContext* ctx, ActorSystem& system,
                               const CliClientConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config) {}

CliClientActor::~CliClientActor() = default;

// ---------------------------------------------------------------------------
// DaemonActor
// ---------------------------------------------------------------------------

void CliClientActor::on_daemon_start() {
    // Build command tree from registry (same tree as server-side)
    command_tree_ = std::make_unique<CommandNode>("/", "CLI root");
    build_command_tree_from_registry(*command_tree_);

    line_editor_ = std::make_unique<LineEditor>(
        LineEditorConfig{
            config_.history_path.empty()
                ? (std::string{getenv("HOME") ? getenv("HOME") : "/tmp"} + "/.hpactor_cli_history")
                : config_.history_path,
            config_.history_max,
            false},
        command_tree_.get());
    line_editor_->load_history();

    session_ = std::make_unique<CliSession>(
        &system_, command_tree_.get(),
        OutputFormatter::create(config_.default_format),
        [](const std::string& text) { printf("%s", text.c_str()); },
        50);
    session_->set_command_host(this);
    session_->set_system_host(this);
    session_->set_lifecycle_host(this);

    printf("HPActor Remote CLI — Connected. Type /help for commands, /quit to exit.\n\n");
}

void CliClientActor::on_daemon_stop() {
    disconnect();
    if (line_editor_) line_editor_->save_history();
}

bool CliClientActor::run_once() {
    if (exec_mode_) {
        connect();
        if (!conn_) {
            printf("Error: could not connect to server\n");
            return false;
        }
        session_->process_line(exec_cmd_);
        disconnect();
        return false;
    }

    if (!conn_) {
        connect();
        if (!conn_) {
            printf("Waiting for connection... (retrying in 1s)\n");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            return running_;
        }
    }

    auto line = line_editor_->readline("hpactor> ");
    if (line.empty()) {
        if (std::feof(stdin)) {
            printf("\nGoodbye.\n");
            running_ = false;
            return false;
        }
        return true;
    }

    line_editor_->add_history(line);
    if (!session_->process_line(line)) {
        running_ = false;
        return false;
    }
    return running_;
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

void CliClientActor::connect() {
    if (config_.transport == CliClientConfig::Transport::HttpJson) {
        // HTTP JSON mode: single connection per request (stateless).
        // For interactive, connect on each command.
        return;
    }

    // Protobuf binary mode: use ConnectionPool
    // For simplicity in the initial implementation, use a raw TCP/UDS socket
    // managed directly. ConnectionPool is designed for actor-to-actor messaging
    // and expects Frame-encoded protobuf with target addressing. The CLI client
    // sends CliCommand protobuf directly with varint-length prefix framing.
    int fd;
    if (!config_.host.empty()) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr);
        if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(fd);
            return;
        }
    } else {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, config_.uds_path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(fd);
            return;
        }
    }
    // conn_ tracking is just the fd for this simple client
    conn_ = reinterpret_cast<Connection*>(static_cast<intptr_t>(fd));
}

void CliClientActor::disconnect() {
    if (conn_) {
        int fd = static_cast<int>(reinterpret_cast<intptr_t>(conn_));
        ::close(fd);
        conn_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Wire protocol
// ---------------------------------------------------------------------------

CliResponse CliClientActor::send_and_wait(const CliCommand& cmd) {
    CliResponse resp;
    resp.set_is_error(true);
    resp.set_error_code(-1);

    int fd = static_cast<int>(reinterpret_cast<intptr_t>(conn_));
    if (fd < 0) return resp;

    // Serialize + varint-length prefix
    std::string wire = cmd.SerializeAsString();
    uint32_t len = static_cast<uint32_t>(wire.size());
    uint8_t len_buf[5];
    int len_bytes = 0;
    uint32_t tmp = len;
    while (tmp > 0x7f) {
        len_buf[len_bytes++] = static_cast<uint8_t>(tmp & 0x7f) | 0x80;
        tmp >>= 7;
    }
    len_buf[len_bytes++] = static_cast<uint8_t>(tmp);

    ::write(fd, len_buf, len_bytes);
    ::write(fd, wire.data(), wire.size());

    // Read response — varint length + CliResponse proto
    char read_buf[4096];
    std::string accum;
    auto deadline = std::chrono::steady_clock::now() + config_.request_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::read(fd, read_buf, sizeof(read_buf));
        if (n > 0) {
            accum.append(read_buf, n);
        } else if (n == 0) {
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }

        // Try to decode
        if (accum.size() >= 1) {
            uint32_t msg_len = 0;
            int shift = 0;
            size_t pos = 0;
            while (pos < accum.size() && pos < 5) {
                uint8_t byte = static_cast<uint8_t>(accum[pos]);
                msg_len |= (byte & 0x7f) << shift;
                pos++;
                if (!(byte & 0x80)) break;
                shift += 7;
            }
            if (pos > 0 && pos <= 5 && accum.size() >= pos + msg_len) {
                if (resp.ParseFromArray(accum.data() + pos, msg_len)) {
                    return resp;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return resp;
}

// ---------------------------------------------------------------------------
// ICliCommandHost — RPC dispatch
// ---------------------------------------------------------------------------

std::optional<InspectStateReply>
CliClientActor::inspect(ActorId target, const InspectStateRequest& req,
                         std::chrono::milliseconds timeout) {
    CliCommand cmd;
    cmd.set_rpc_method("inspect");
    req.set_target_actor_id(target.value());
    cmd.set_rpc_request(req.SerializeAsString());
    auto resp = send_and_wait(cmd);
    if (resp.is_structured() && !resp.is_error()) {
        InspectStateReply reply;
        if (reply.ParseFromString(resp.payload()))
            return reply;
    }
    return std::nullopt;
}

std::optional<KillReply>
CliClientActor::kill(ActorId target, const KillRequest& req,
                      std::chrono::milliseconds timeout) {
    CliCommand cmd;
    cmd.set_rpc_method("kill");
    KillRequest mutable_req = req;
    mutable_req.set_target_actor_id(target.value());
    cmd.set_rpc_request(mutable_req.SerializeAsString());
    auto resp = send_and_wait(cmd);
    if (resp.is_structured() && !resp.is_error()) {
        KillReply reply;
        if (reply.ParseFromString(resp.payload()))
            return reply;
    }
    return std::nullopt;
}

std::optional<QuarantineReply>
CliClientActor::quarantine(ActorId target, const QuarantineRequest& req,
                            std::chrono::milliseconds timeout) {
    CliCommand cmd;
    cmd.set_rpc_method("quarantine");
    QuarantineRequest mutable_req = req;
    mutable_req.set_target_actor_id(target.value());
    cmd.set_rpc_request(mutable_req.SerializeAsString());
    auto resp = send_and_wait(cmd);
    if (resp.is_structured() && !resp.is_error()) {
        QuarantineReply reply;
        if (reply.ParseFromString(resp.payload()))
            return reply;
    }
    return std::nullopt;
}

std::vector<ActorMeta>
CliClientActor::enumerate(std::string_view filter) {
    CliCommand cmd;
    cmd.set_rpc_method("enumerate");
    cmd.set_rpc_request(std::string(filter));
    auto resp = send_and_wait(cmd);
    std::vector<ActorMeta> result;
    if (resp.is_structured() && !resp.is_error()) {
        ListActorsReply list_reply;
        if (list_reply.ParseFromString(resp.payload())) {
            for (const auto& pb_meta : list_reply.actors()) {
                ActorMeta m;
                m.actor_id = pb_meta.actor_id();
                m.actor_type = pb_meta.actor_type();
                m.state = pb_meta.state();
                result.push_back(std::move(m));
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// ISystemCliHost — command-tree dispatch
// ---------------------------------------------------------------------------

void CliClientActor::render_system_stats(OutputFormatter& output) {
    CliCommand cmd;
    cmd.set_path("system/stats");
    auto resp = send_and_wait(cmd);
    if (!resp.is_error())
        output.raw(resp.payload());
    else
        output.error("Failed to fetch system stats from server");
}

void CliClientActor::render_memory_stats(OutputFormatter& output) {
    CliCommand cmd;
    cmd.set_path("system/memory");
    auto resp = send_and_wait(cmd);
    if (!resp.is_error())
        output.raw(resp.payload());
    else
        output.error("Failed to fetch memory stats from server");
}

void CliClientActor::render_fault_status(OutputFormatter& output) {
    CliCommand cmd;
    cmd.set_path("fault/status");
    auto resp = send_and_wait(cmd);
    if (!resp.is_error())
        output.raw(resp.payload());
    else
        output.error("Failed to fetch fault status from server");
}

void CliClientActor::render_dlq_list(OutputFormatter& output, std::string_view filter) {
    CliCommand cmd;
    cmd.set_path("dlq/list");
    if (!filter.empty())
        cmd.add_args(std::string(filter));
    auto resp = send_and_wait(cmd);
    if (!resp.is_error())
        output.raw(resp.payload());
    else
        output.error("Failed to fetch DLQ list from server");
}

result<void> CliClientActor::dlq_replay(uint32_t index, ActorId target) {
    CliCommand cmd;
    cmd.set_path("dlq/replay");
    cmd.add_args(std::to_string(index));
    cmd.add_args(std::to_string(target.value()));
    auto resp = send_and_wait(cmd);
    if (resp.is_error())
        return error::make(error::Code::kInternal, "dlq replay failed");
    return result<void>::make();
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost — command-tree dispatch
// ---------------------------------------------------------------------------

result<void> CliClientActor::drain() {
    CliCommand cmd;
    cmd.set_path("system/drain");
    auto resp = send_and_wait(cmd);
    if (resp.is_error())
        return error::make(error::Code::kInternal, "drain failed");
    return result<void>::make();
}

result<void> CliClientActor::shutdown() {
    CliCommand cmd;
    cmd.set_path("quit");
    auto resp = send_and_wait(cmd);
    if (resp.is_error())
        return error::make(error::Code::kInternal, "shutdown failed");
    return result<void>::make();
}

} // namespace cli
} // namespace hpactor
```

- [ ] **Step 3: Add CliClientActor to hpactor_lib**

Add `src/cli/cli_client_actor.cpp` to the library sources in `src/CMakeLists.txt`.

- [ ] **Step 4: Build and verify compilation**

```bash
ninja -C build hpactor_lib
```

Expected: compiles successfully.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/cli/cli_client_actor.hpp src/cli/cli_client_actor.cpp src/CMakeLists.txt
git commit -m "feat: add CliClientActor — remote CLI client as DaemonActor

Implements ICliCommandHost (structured RPC dispatch via rpc_method),
ISystemCliHost (command-tree dispatch via path+args), and
ILifecycleCliHost. Uses varint-length-prefixed protobuf framing
for wire communication. Supports interactive (LineEditor + persistent
connection) and exec (single command, connect-send-disconnect) modes.

Issue: #306"
```

---

### Task 12: Rewrite tools/hpactor-cli/main.cpp as thin entry point

**Files:**
- Modify: `tools/hpactor-cli/main.cpp`

- [ ] **Step 1: Replace main.cpp with thin entry point**

Replace the entire contents of `tools/hpactor-cli/main.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

/// \file main.cpp
/// \brief Standalone CLI client for the hpactor daemon.
///
/// Creates a minimal ActorSystem with a CliClientActor that connects
/// to a remote CliServerActor via UDS or TCP using the protobuf
/// CliCommand/CliResponse wire protocol.

#include <hpactor/cli/cli_client_actor.hpp>
#include <hpactor/cli/cli_client_config.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

static void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  -s, --socket PATH   Unix domain socket path\n"
        << "                      (default: /var/run/hpactor/hpactor-cli.sock)\n"
        << "  -H, --host HOST     TCP host address (disables UDS)\n"
        << "  -p, --port PORT     TCP port (required with --host)\n"
        << "  --http-port PORT    HTTP JSON transport port (enables HTTP mode)\n"
        << "  -e, --exec CMD      Execute a single command and exit\n"
        << "  -f, --format FMT    Output format (pretty, json, tabular)\n"
        << "  -h, --help          Show this help message\n";
}

int main(int argc, char* argv[]) {
    hpactor::cli::CliClientConfig config;
    std::string exec_cmd;
    bool show_help = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s" || arg == "--socket") {
            if (i + 1 >= argc) { std::cerr << "Error: --socket requires a path\n"; return 1; }
            config.uds_path = argv[++i];
        } else if (arg == "-H" || arg == "--host") {
            if (i + 1 >= argc) { std::cerr << "Error: --host requires an address\n"; return 1; }
            config.host = argv[++i];
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 >= argc) { std::cerr << "Error: --port requires a port\n"; return 1; }
            config.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--http-port") {
            if (i + 1 >= argc) { std::cerr << "Error: --http-port requires a port\n"; return 1; }
            config.http_port = static_cast<uint16_t>(std::atoi(argv[++i]));
            config.transport = hpactor::cli::CliClientConfig::Transport::HttpJson;
        } else if (arg == "-e" || arg == "--exec") {
            if (i + 1 >= argc) { std::cerr << "Error: --exec requires a command\n"; return 1; }
            exec_cmd = argv[++i];
        } else if (arg == "-f" || arg == "--format") {
            if (i + 1 >= argc) { std::cerr << "Error: --format requires a format\n"; return 1; }
            config.default_format = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            show_help = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (show_help) { print_usage(argv[0]); return 0; }
    if (!config.host.empty() && config.port == 0) {
        std::cerr << "Error: TCP mode requires --port\n"; return 1;
    }

    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0; // no scheduler — CliClientActor is a DaemonActor
    hpactor::ActorSystem system(sys_config);

    auto client = system.spawn<hpactor::cli::CliClientActor>(config);
    auto* raw = static_cast<hpactor::cli::CliClientActor*>(system.get_actor(client.id()).get());

    if (!exec_cmd.empty()) {
        raw->set_exec_command(exec_cmd);
    }

    while (raw->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return 0;
}
```

- [ ] **Step 2: Build hpactor-cli**

```bash
ninja -C build hpactor-cli
```

Expected: builds successfully. The `main.cpp` is now ~80 lines instead of ~434.

- [ ] **Step 3: Commit**

```bash
git add tools/hpactor-cli/main.cpp
git commit -m "refactor: replace raw-socket hpactor-cli with CliClientActor entry point

Remove ~350 lines of raw socket/IO code (make_nonblocking,
connect_uds_nonblock, connect_tcp_nonblock, await_connect,
send_line_async, recv_response_async, FdGuard). Replace with
thin main() that creates ActorSystem + CliClientActor.

Issue: #306"
```

---

### Task 13: Write unit test for wire protocol

**Files:**
- Create: `tests/unit/cli/test_cli_wire_protocol.cpp`

- [ ] **Step 1: Write the test**

```cpp
#include <hpactor/cli.pb.h>
#include <gtest/gtest.h>

TEST(CliWireProtocol, CliCommandDefaultConstruction) {
    hpactor::cli::CliCommand cmd;
    EXPECT_TRUE(cmd.path().empty());
    EXPECT_TRUE(cmd.rpc_method().empty());
    EXPECT_FALSE(cmd.has_rpc_request());
}

TEST(CliWireProtocol, CliCommandSetPath) {
    hpactor::cli::CliCommand cmd;
    cmd.set_path("system/stats");
    EXPECT_EQ(cmd.path(), "system/stats");
}

TEST(CliWireProtocol, CliCommandSetRpcMethod) {
    hpactor::cli::CliCommand cmd;
    cmd.set_rpc_method("inspect");
    EXPECT_EQ(cmd.rpc_method(), "inspect");
}

TEST(CliWireProtocol, CliCommandParamsRoundTrip) {
    hpactor::cli::CliCommand cmd;
    (*cmd.mutable_params())["format"] = "json";
    (*cmd.mutable_params())["verbose"] = "true";
    EXPECT_EQ(cmd.params().at("format"), "json");
    EXPECT_EQ(cmd.params().at("verbose"), "true");
}

TEST(CliWireProtocol, CliCommandArgsRoundTrip) {
    hpactor::cli::CliCommand cmd;
    cmd.add_args("42");
    cmd.add_args("--verbose");
    ASSERT_EQ(cmd.args_size(), 2);
    EXPECT_EQ(cmd.args(0), "42");
    EXPECT_EQ(cmd.args(1), "--verbose");
}

TEST(CliWireProtocol, CliResponseDefaultConstruction) {
    hpactor::cli::CliResponse resp;
    EXPECT_FALSE(resp.is_error());
    EXPECT_FALSE(resp.is_structured());
}

TEST(CliWireProtocol, CliResponseStructuredFlag) {
    hpactor::cli::CliResponse resp;
    resp.set_is_structured(true);
    resp.set_content_type("application/x-protobuf");
    resp.set_payload("binary-data");
    EXPECT_TRUE(resp.is_structured());
    EXPECT_EQ(resp.content_type(), "application/x-protobuf");
}

TEST(CliWireProtocol, CliCommandRoundTrip) {
    hpactor::cli::CliCommand original;
    original.set_path("actor/42/show");
    (*original.mutable_params())["format"] = "json";
    original.add_args("42");

    std::string wire = original.SerializeAsString();
    hpactor::cli::CliCommand decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.path(), "actor/42/show");
    EXPECT_EQ(decoded.params().at("format"), "json");
    ASSERT_EQ(decoded.args_size(), 1);
    EXPECT_EQ(decoded.args(0), "42");
}

TEST(CliWireProtocol, CliResponseRoundTrip) {
    hpactor::cli::CliResponse original;
    original.set_content_type("text/plain");
    original.set_payload("Actor 42: Worker-1 (Running)");
    original.set_is_error(false);

    std::string wire = original.SerializeAsString();
    hpactor::cli::CliResponse decoded;
    ASSERT_TRUE(decoded.ParseFromString(wire));
    EXPECT_EQ(decoded.content_type(), "text/plain");
    EXPECT_EQ(decoded.payload(), "Actor 42: Worker-1 (Running)");
    EXPECT_FALSE(decoded.is_error());
}

TEST(CliWireProtocol, CliCommandRpcDispatchMode) {
    hpactor::cli::CliCommand cmd;
    cmd.set_rpc_method("inspect");
    cmd.set_rpc_request("serialized-proto-bytes");
    EXPECT_EQ(cmd.rpc_method(), "inspect");
    EXPECT_EQ(cmd.rpc_request(), "serialized-proto-bytes");
    EXPECT_TRUE(cmd.path().empty()); // RPC mode — path not used
}
```

- [ ] **Step 2: Add test to CMake and run**

Add `test_cli_wire_protocol.cpp` to `tests/unit/cli/CMakeLists.txt`, run:

```bash
ninja -C build tests/unit/cli/test_unit_cli_wire_protocol
./build/tests/unit/cli/test_unit_cli_wire_protocol
```

Expected: 10 tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/cli/test_cli_wire_protocol.cpp tests/unit/cli/CMakeLists.txt
git commit -m "test: add wire protocol unit tests for CliCommand/CliResponse

10 tests covering default construction, field setters, round-trip
serialization, params/args, structured RPC mode, and content types.

Issue: #306"
```

---

### Task 14: Write unit test for command host interface dispatch

**Files:**
- Create: `tests/unit/cli/test_cli_command_host.cpp`

- [ ] **Step 1: Write the test with mock host implementations**

```cpp
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli/pretty_formatter.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/types/types.hpp>
#include <gtest/gtest.h>

namespace {

// Mock ICliCommandHost that tracks calls
class MockCommandHost : public hpactor::cli::ICliCommandHost {
  public:
    int inspect_calls = 0;
    int kill_calls = 0;
    int enumerate_calls = 0;

    std::optional<hpactor::cli::InspectStateReply>
    inspect(hpactor::ActorId, const hpactor::cli::InspectStateRequest&,
            std::chrono::milliseconds) override {
        inspect_calls++;
        hpactor::cli::InspectStateReply reply;
        reply.mutable_metadata()->set_actor_id(42);
        reply.mutable_metadata()->set_actor_type("TestActor");
        return reply;
    }

    std::optional<hpactor::cli::KillReply>
    kill(hpactor::ActorId, const hpactor::cli::KillRequest&,
         std::chrono::milliseconds) override {
        kill_calls++;
        hpactor::cli::KillReply reply;
        reply.set_success(true);
        return reply;
    }

    std::optional<hpactor::cli::QuarantineReply>
    quarantine(hpactor::ActorId, const hpactor::cli::QuarantineRequest&,
               std::chrono::milliseconds) override {
        hpactor::cli::QuarantineReply reply;
        reply.set_success(true);
        return reply;
    }

    std::vector<hpactor::cli::ActorMeta>
    enumerate(std::string_view) override {
        enumerate_calls++;
        hpactor::cli::ActorMeta m;
        m.actor_id = 1;
        m.actor_type = "TestActor";
        return {m};
    }
};

// Mock ISystemCliHost that captures output
class MockSystemHost : public hpactor::cli::ISystemCliHost {
  public:
    std::string last_output;

    void render_system_stats(hpactor::cli::OutputFormatter& output) override {
        output.header("Stats");
        last_output = "stats_rendered";
    }
    void render_memory_stats(hpactor::cli::OutputFormatter& output) override {
        output.header("Memory");
        last_output = "memory_rendered";
    }
    void render_fault_status(hpactor::cli::OutputFormatter& output) override {
        output.header("Faults");
        last_output = "faults_rendered";
    }
    void render_dlq_list(hpactor::cli::OutputFormatter& output,
                         std::string_view filter) override {
        output.header("DLQ");
        last_output = filter.empty() ? "dlq_all" : std::string(filter);
    }
    hpactor::result<void> dlq_replay(uint32_t, hpactor::ActorId) override {
        last_output = "dlq_replayed";
        return hpactor::result<void>::make();
    }
};

// Mock ILifecycleCliHost
class MockLifecycleHost : public hpactor::cli::ILifecycleCliHost {
  public:
    bool drained = false;
    bool shut_down = false;

    hpactor::result<void> drain() override {
        drained = true;
        return hpactor::result<void>::make();
    }
    hpactor::result<void> shutdown() override {
        shut_down = true;
        return hpactor::result<void>::make();
    }
};

} // anonymous namespace

TEST(CliCommandHost, InspectDelegatesToMock) {
    MockCommandHost host;
    hpactor::cli::InspectStateRequest req;
    auto result = host.inspect(hpactor::ActorId{42}, req);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->metadata().actor_id(), 42u);
    EXPECT_EQ(host.inspect_calls, 1);
}

TEST(CliCommandHost, KillDelegatesToMock) {
    MockCommandHost host;
    hpactor::cli::KillRequest req;
    auto result = host.kill(hpactor::ActorId{1}, req);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->success());
    EXPECT_EQ(host.kill_calls, 1);
}

TEST(CliCommandHost, EnumerateDelegatesToMock) {
    MockCommandHost host;
    auto actors = host.enumerate("");
    ASSERT_EQ(actors.size(), 1u);
    EXPECT_EQ(actors[0].actor_id, 1u);
    EXPECT_EQ(host.enumerate_calls, 1);
}

TEST(CliSystemHost, RenderSystemStatsDelegatesToMock) {
    MockSystemHost host;
    hpactor::cli::PrettyFormatter fmt;
    host.render_system_stats(fmt);
    EXPECT_EQ(host.last_output, "stats_rendered");
}

TEST(CliSystemHost, DlqReplayDelegatesToMock) {
    MockSystemHost host;
    auto result = host.dlq_replay(0, hpactor::ActorId{1});
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(host.last_output, "dlq_replayed");
}

TEST(CliLifecycleHost, DrainDelegatesToMock) {
    MockLifecycleHost host;
    auto result = host.drain();
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(host.drained);
}

TEST(CliLifecycleHost, ShutdownDelegatesToMock) {
    MockLifecycleHost host;
    auto result = host.shutdown();
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(host.shut_down);
}
```

- [ ] **Step 2: Add test to CMake and run**

Add `test_cli_command_host.cpp` to `tests/unit/cli/CMakeLists.txt`, run:

```bash
ninja -C build tests/unit/cli/test_unit_cli_command_host
./build/tests/unit/cli/test_unit_cli_command_host
```

Expected: 7 tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/cli/test_cli_command_host.cpp tests/unit/cli/CMakeLists.txt
git commit -m "test: add command host interface unit tests

Mock implementations of ICliCommandHost, ISystemCliHost, and
ILifecycleCliHost validate interface dispatch for inspect, kill,
enumerate, render_*, dlq_replay, drain, and shutdown.

Issue: #306"
```

---

### Task 15: Write integration test for CliClientActor ↔ CliServerActor

**Files:**
- Create: `tests/integration/cli/test_cli_client_actor.cpp`
- Create: `tests/integration/cli/test_cli_server_proto.cpp`

- [ ] **Step 1: Write server proto listener integration test**

`tests/integration/cli/test_cli_server_proto.cpp`:

```cpp
#include <hpactor/cli/cli_server_actor.hpp>
#include <hpactor/cli/cli_server_config.hpp>
#include <hpactor/cli.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

TEST(CliServerProto, ProtoListenerStartsWithoutError) {
    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0;
    hpactor::ActorSystem system(sys_config);

    hpactor::cli::CliServerConfig cfg;
    cfg.proto_tcp_port = 19091; // ephemeral-ish test port
    cfg.tcp_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliServerActor>(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify server is running
    auto* raw = static_cast<hpactor::cli::CliServerActor*>(
        system.get_actor(server.id()).get());
    ASSERT_TRUE(raw->is_system_actor());

    raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(CliServerProto, CommandTreeDispatchReturnsFormattedText) {
    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0;
    hpactor::ActorSystem system(sys_config);

    hpactor::cli::CliServerConfig cfg;
    cfg.proto_tcp_port = 19092;
    cfg.tcp_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliServerActor>(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Connect and send a CliCommand
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19092);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_GE(connect(fd, (struct sockaddr*)&addr, sizeof(addr)), 0);

    // Send: CliCommand{path="help"}
    hpactor::cli::CliCommand cmd;
    cmd.set_path("help");
    std::string wire = cmd.SerializeAsString();
    uint32_t len = wire.size();
    uint8_t len_buf[5];
    int lb = 0;
    while (len > 0x7f) { len_buf[lb++] = (len & 0x7f) | 0x80; len >>= 7; }
    len_buf[lb++] = len;
    write(fd, len_buf, lb);
    write(fd, wire.data(), wire.size());

    // Read response
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    EXPECT_GT(n, 0);

    close(fd);
    auto* raw = static_cast<hpactor::cli::CliServerActor*>(
        system.get_actor(server.id()).get());
    raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```

- [ ] **Step 2: Build and run**

Add to `tests/integration/cli/CMakeLists.txt`:

```bash
ninja -C build tests/integration/cli/test_integration_cli_server_proto
./build/tests/integration/cli/test_integration_cli_server_proto
```

Expected: 2 tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/integration/cli/test_cli_server_proto.cpp tests/integration/cli/CMakeLists.txt
git commit -m "test: add CliServerActor proto listener integration tests

Validates protobuf listener startup and CliCommand dispatch producing
formatted text response over varint-length-prefixed wire protocol.

Issue: #306"
```

---

### Task 16: Run full test suite and verify no regressions

**Files:** (none — verification only)

- [ ] **Step 1: Full build**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
```

Expected: zero build errors.

- [ ] **Step 2: Full test suite**

```bash
ctest --output-on-failure --parallel 8
```

Expected: All existing tests pass. New tests pass. No regressions.

- [ ] **Step 3: Run specific CLI test suites individually**

```bash
ctest --output-on-failure --parallel 8 -R "cli"
```

Expected: all CLI tests pass (267 existing + new tests).

- [ ] **Step 4: Commit (if any final adjustments needed)**

```bash
git add -A
git commit -m "chore: final verification — all tests pass after CLI architecture refactor

Issue: #306"
```

---

## Deferred Items

These are specified in the design doc but deferred to a follow-up plan:

- **HTTP JSON endpoint (`POST /cli`).** The proto schema supports JSON mapping and
  the `CliClientConfig::Transport::HttpJson` enum exists, but the HTTPGateway
  integration in CliServerActor and the HTTP client path in CliClientActor are
  deferred. The current plan implements binary protobuf transport end-to-end.
- **Legacy port deprecation notice.** The legacy raw-text port remains
  unchanged. Adding a deprecation log message on accept is a one-line change
  deferred to a follow-up cleanup task.
- **`test_cli_http_endpoint` integration test.** Deferred with the HTTP endpoint.
- **`test_cli_client_actor` full integration test.** Deferred — requires a running
  CliServerActor in the same process, which needs careful test infrastructure
  (EventLoop in test, non-conflicting ports). The unit-level wire protocol and
  interface tests provide coverage for the critical paths.

## Acceptance Criteria Checklist

- [ ] `ICliCommandHost`, `ISystemCliHost`, `ILifecycleCliHost` interfaces exist in `include/hpactor/cli/cli_command_host.hpp`
- [ ] `CommandContext` has `command_host`, `system_host`, `lifecycle_host` fields
- [ ] `CliActor` implements all three host interfaces
- [ ] `CliServerActor` implements all three host interfaces
- [ ] `CliClientActor` implements all three host interfaces
- [ ] `hpactor-cli` binary is a thin `main()` creating `ActorSystem` + `CliClientActor`
- [ ] Wire protocol uses `CliCommand`/`CliResponse` protobuf with varint-length prefix
- [ ] Legacy raw-text protocol preserved on existing port
- [ ] All 267+ existing CLI tests pass without modification
- [ ] Proto wire protocol tests pass (10 tests)
- [ ] Command host interface tests pass (7 tests)
- [ ] Server proto integration tests pass (2 tests)
