# CLI Architecture Standardization: Command Host Interface, Client Actor, Protobuf Wire Protocol

**Date:** 2026-06-15
**Issue:** [#306](https://github.com/skg7on/HPActor/issues/306)
**Status:** Approved

## Motivation

`tools/hpactor-cli/` is a 434-line standalone binary that is **not an actor**. It
manually creates raw sockets with `socket()`/`connect()`/`fcntl()`, reimplements
async connect/send/recv with busy-poll loops via `EventLoop`, and uses an ad-hoc
raw-text wire protocol (`<command>\n` → `<response>\0`). It duplicates connection
management, event-loop I/O, and line-editing — all of which already exist in the
framework through `Connection`, `TcpTransport`, `ConnectionPool`, `CliSession`,
and `LineEditor`.

Meanwhile `apps/cli_demo/` uses `CliActor` (a `DaemonActor`) with `CliSession`,
`ICommand` self-registration, and `CommandNode` trie dispatch — a clean,
idiomatic actor architecture. But the two code paths share nothing.

### Core Problems

1. **`hpactor-cli` is not an actor.** It bypasses `DaemonActor`, `ConnectionPool`,
   `TcpTransport`, `CliSession`, and `ICommand`, writing ~350 lines of raw socket
   C code that reimplements patterns the framework already provides.

2. **No standardized command host interface.** `CommandContext` uses a union of
   concrete pointers (`CliActor*` + `CliServerActor*`). Every command handler
   that needs request-response must check which is non-null. Adding a remote CLI
   client actor would require a third pointer.

3. **Ad-hoc wire protocol.** Fragile text with `\n`/`\0` delimiters, no
   versioning, no structured data. The framework already has protobuf for typed
   message serialization.

4. **Duplicated connection infrastructure.** `make_nonblocking()`,
   `await_connect()`, `send_line_async()`, `recv_response_async()` all
   reimplement patterns already in `Connection`/`TcpTransport`/`EventLoop`.

5. **Inconsistent command registration.** Most commands use `ICommand` +
   `CommandRegistration<T>`, but `ask_commands` uses a separate
   `register_*()` function outside the registry.

6. **Commands tightly coupled to local queries.** `SystemStatsCommand` directly
   accesses `ctx.system->actor_count()`. On a remote client, `ctx.system` is
   null — commands must work through a host interface that abstracts local vs.
   remote.

## Design

### Key Invariant

The only difference between `cli_demo` and `hpactor-cli` is which
`ICliCommandHost` implementation is wired in. The command tree (`ICommand`),
session dispatch (`CliSession`), output formatting (`OutputFormatter`), paging
(`Pager`), and line editing (`LineEditor`) are identical. All commands work on
local and remote hosts with zero branching.

### 1. Layered Command Host Interfaces

Three segregated interfaces replace the `CliActor*`/`CliServerActor*` union in
`CommandContext`. Layering follows the architecture's reliability-plane
boundaries (data plane, operations plane, lifecycle).

**New header:** `include/hpactor/cli/cli_command_host.hpp`

```cpp
// Core — actor-level operations every host must support
class ICliCommandHost {
public:
  virtual ~ICliCommandHost() = default;

  virtual std::optional<InspectStateReply>
  inspect(ActorId target, const InspectStateRequest& req,
          std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) = 0;

  virtual std::optional<KillReply>
  kill(ActorId target, const KillRequest& req,
       std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) = 0;

  virtual std::optional<QuarantineReply>
  quarantine(ActorId target, const QuarantineRequest& req,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) = 0;

  virtual std::vector<ActorMeta>
  enumerate(std::string_view filter = "") = 0;
};

// System — aggregate queries, observability
//
// Methods take an OutputFormatter& so the host can either:
//   - Local:  query ActorSystem and render directly (e.g. output.key_value(...))
//   - Remote: send CliCommand over wire, receive CliResponse,
//             write payload via output.raw(response.payload)
// Commands never branch on local vs. remote.
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

// Lifecycle — drain, stop, shutdown
class ILifecycleCliHost {
public:
  virtual ~ILifecycleCliHost() = default;

  virtual result<void> drain() = 0;
  virtual result<void> shutdown() = 0;
};
```

**Updated CommandContext** (`include/hpactor/cli/command_context.hpp`):

```cpp
struct CommandContext {
  std::vector<std::string> args;
  std::map<std::string, std::string> params;

  // Before: CliActor* cli_actor + CliServerActor* cli_server_actor (concrete union)
  // After:  three segregated interfaces
  ICliCommandHost*   command_host   = nullptr;
  ISystemCliHost*    system_host    = nullptr;
  ILifecycleCliHost* lifecycle_host = nullptr;

  // Kept for direct ActorSystem access when available (nullable on remote)
  ActorSystem*       system         = nullptr;

  CliSession*        cli_session    = nullptr;
  OutputFormatter*   output         = nullptr;
  bool               paged          = false;
  uint32_t           page_size      = 50;
  std::string        format         = "pretty";

  // Legacy — kept for migration period, populate from host interfaces.
  // Remove once all commands use the interface pointers.
  CliActor*       cli_actor        = nullptr;  // deprecated
  CliServerActor* cli_server_actor = nullptr;  // deprecated

  // ... existing has_flag(), get_param() helpers unchanged ...
};
```

**Who implements what:**

| Host Type | ICliCommandHost | ISystemCliHost | ILifecycleCliHost |
|-----------|:---:|:---:|:---:|
| **CliActor** (stdin) | Local dispatch via mailbox | Render via ActorSystem queries | `system.shutdown()` |
| **CliServerActor** (socket server) | Local dispatch via mailbox | Render via ActorSystem queries | `system.shutdown()` |
| **CliClientActor** (remote, NEW) | Serialize → send → deserialize reply | Send CliCommand, write response payload to output | Send CliCommand, write response payload |

### 2. Wire Protocol

A single generic protobuf message pair replaces the ad-hoc raw text protocol.
One proto schema, dual serialization (binary for UDS/TCP, JSON for HTTP).

**New proto file:** `proto/hpactor/cli.proto`

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

**Framing:**

- **Protobuf binary (UDS/TCP):** Standard varint-length-prefixed protobuf frames.
  Reuses existing `hpactor_lib` serialization primitives (same pattern as Frame
  encode/decode in the transport layer). No custom framing needed.

- **HTTP JSON (TCP):** Standard `google.protobuf.util.JsonFormat` mapping.
  `POST /cli` with `Content-Type: application/json`. Response is `200 OK` with
  JSON body. `bytes payload` maps to base64-encoded JSON string.

**JSON mapping example:**

```json
// Request
{
  "path": "actor/42/show",
  "params": {"format": "json"},
  "args": [],
  "format": "pretty"
}

// Response
{
  "content_type": "text/plain",
  "payload": "QWN0b3IgNDI6IFdvcmtlci0x...",
  "is_error": false
}
```

### 3. CliClientActor — Remote CLI Client

A new `DaemonActor` subclass that implements all three host interfaces by
serializing to `CliCommand` protobuf, sending over the wire, and deserializing
the `CliResponse`. Replaces the 434-line `tools/hpactor-cli/main.cpp` with a
proper actor (~300 lines) + a thin main (~50 lines).

**New header:** `include/hpactor/cli/cli_client_actor.hpp`

```cpp
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

  // ICliCommandHost — use RPC dispatch (rpc_method + rpc_request fields)
  // Send CliCommand with rpc_method="inspect", rpc_request=<serialized request>.
  // Receive CliResponse with is_structured=true, payload=<serialized reply>.
  std::optional<InspectStateReply> inspect(...) override;
  std::optional<KillReply> kill(...) override;
  std::optional<QuarantineReply> quarantine(...) override;
  std::vector<ActorMeta> enumerate(std::string_view filter) override;

  // ISystemCliHost — use command-tree dispatch (path + params + args fields)
  // Send CliCommand with path="system/stats". Receive CliResponse with
  // formatted text payload. Write to output.
  void render_system_stats(OutputFormatter& output) override;
  void render_memory_stats(OutputFormatter& output) override;
  void render_fault_status(OutputFormatter& output) override;
  void render_dlq_list(OutputFormatter& output, std::string_view filter) override;
  result<void> dlq_replay(uint32_t index, ActorId target) override;

  // ILifecycleCliHost
  result<void> drain() override;
  result<void> shutdown() override;

private:
  void connect();                              // UDS or TCP via ConnectionPool
  void disconnect();                           // clean close + pool cleanup
  CliResponse send_and_wait(const CliCommand&); // serialize → send → poll mailbox

  CliClientConfig config_;
  std::unique_ptr<CliSession> session_;         // command dispatch (same as CliActor)
  std::unique_ptr<LineEditor> line_editor_;     // interactive input
  std::unique_ptr<ConnectionPool> pool_;        // connection management
  Connection* conn_ = nullptr;                  // active connection
  bool running_ = true;
  bool exec_mode_ = false;
  std::string exec_cmd_;
};
```

**`run_once()` flow:**

```cpp
bool CliClientActor::run_once() {
  if (exec_mode_) {
    // --exec: connect, send one command, print response, exit
    connect();
    auto response = session_->process_line(exec_cmd_);
    disconnect();
    return false;
  }

  // Interactive: ensure connection, read line, dispatch
  if (!conn_ || !conn_->is_connected()) {
    connect();  // with exponential backoff (1s → 30s, jitter)
  }

  auto line = line_editor_->readline("> ");
  if (line.empty()) return false;

  line_editor_->add_history(line);
  session_->process_line(line);
  return running_;
}
```

**Connection model:**

| Mode | Trigger | Connection lifecycle |
|------|---------|---------------------|
| Interactive | `hpactor-cli` (no `--exec`) | Connect once, keep-alive, reconnect on drop with 1s→30s exponential backoff + jitter |
| Exec | `hpactor-cli --exec "/actor list"` | Connect → send → receive → disconnect → exit. 5s connect timeout. |
| HTTP | `curl -X POST :9090/cli -d '{...}'` | Stateless — each POST is a full request-response cycle |

**CliClientConfig** (`include/hpactor/cli/cli_client_config.hpp`):

```cpp
struct CliClientConfig {
  // Transport
  std::string uds_path    = "/var/run/hpactor/hpactor-cli.sock";
  std::string host;                     // empty = use UDS
  uint16_t    port        = 0;          // required with host
  uint16_t    http_port   = 0;          // optional HTTP JSON transport

  enum class Transport { Protobuf, HttpJson };
  Transport transport = Transport::Protobuf;

  // Session
  std::string history_path;             // "" = $HOME/.hpactor_cli_history
  uint32_t    history_max    = 1000;
  std::string default_format = "pretty";

  // Timing
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds request_timeout{10000};

  // Reconnection
  std::chrono::milliseconds reconnect_min{1000};
  std::chrono::milliseconds reconnect_max{30000};
};
```

### 4. Server-Side Actor Decomposition

The monolithic `CliServerActor` is split into three focused actors, each
handling one wire protocol. All three share one `CliSession` + `CommandNode`
tree via the host interfaces.

#### 4a. CliServerActor — Legacy Raw Text (Unchanged, Deprecated)

Keeps the existing `UDS`/`TCP` acceptor with raw `::read()`/`::write()`.
Protocol: newline-delimited commands, NUL-terminated responses. No new
features — this is preserved for backward compatibility only.

**CliServerConfig** (stripped of proto/HTTP fields):

```cpp
struct CliServerConfig {
  std::string uds_listen_path;
  uint16_t    tcp_listen_port   = 0;
  std::string tcp_bind_address  = "127.0.0.1";
  uint32_t    max_sessions      = 16;
  std::chrono::milliseconds session_timeout{300000};
  std::string default_format    = "pretty";
  uint32_t    page_size         = 50;
  uint32_t    uds_socket_mode   = 0660;
  std::string uds_socket_owner;
  std::string uds_socket_group;
};
```

#### 4b. CliProtoServerActor — Protobuf CLI Server (NEW)

Reuses `TcpTransport::listen()` for both UDS and TCP. Accepted connections
are automatically wrapped in `WireFrameConnection` instances, which handle
HPAC Frame decode/encode transparently. No manual frame parsing.

**New header:** `include/hpactor/cli/cli_proto_server_actor.hpp`

```
class CliProtoServerActor : public DaemonActor,
                            public ICliCommandHost,
                            public ISystemCliHost,
                            public ILifecycleCliHost {
```

Key design:
- Owns an `EventLoop` + `TcpTransport` for listening
- Each accepted connection gets a `CliSession` with `output_fn` that
  encodes response via HPAC Frame and calls `conn->send()`
- `WireFrameConnection::handle_read()` auto-decodes HPAC frames
- `frame_handler` receives decoded `CliCommand` protobuf → dispatches
  via `CliSession::process_line()`
- Structured RPC (`rpc_method` set) bypasses command tree, calls host
  interface directly

**Wire protocol:**
```
Client:  HPAC Frame (magic "HPAC" + big-endian len + CliCommand proto)
Server:  HPAC Frame (magic "HPAC" + big-endian len + CliResponse proto)
```

**CliProtoServerConfig:**

```cpp
struct CliProtoServerConfig {
  std::string uds_listen_path;           // e.g. "/var/run/hpactor/hpactor-cli.sock"
  uint16_t    tcp_listen_port   = 0;     // e.g. 9091
  std::string tcp_bind_address  = "127.0.0.1";
  uint32_t    max_sessions      = 16;
  std::chrono::milliseconds session_timeout{300000};
  std::string default_format    = "pretty";
  uint32_t    page_size         = 50;
  uint32_t    uds_socket_mode   = 0660;
  std::string uds_socket_owner;
  std::string uds_socket_group;
};
```

#### 4c. CliHttpServerActor — HTTP JSON CLI Server (NEW)

Reuses the existing `HTTPGateway` infrastructure (same pattern as
`HealthHttpServer` and `HTTPGatewayActor`). Accepts `POST /cli` with
JSON body, routes to `CliSession`, returns JSON response.

**New header:** `include/hpactor/cli/cli_http_server_actor.hpp`

```
class CliHttpServerActor : public DaemonActor,
                           public ISystemCliHost,
                           public ILifecycleCliHost {
```

Key design:
- Extends `DaemonActor` — runs on dedicated thread
- Owns an `HTTPGateway` for listen/accept/HTTP parsing/response formatting
- Routes `POST /cli` → JSON-decode `CliCommand` → `CliSession::process_line()`
  → encode `CliResponse` as JSON → HTTP 200 response
- `ICliCommandHost` is NOT implemented (no structured RPC over HTTP —
  command-tree dispatch only via `path` + `params` + `args`)
- Enables any HTTP client (curl, Python, browser) to interact with the CLI
  with zero HPActor dependency

**HTTP endpoint:**

```
POST /cli HTTP/1.1
Content-Type: application/json

{"path": "system/stats", "params": {"format": "json"}}

→ 200 OK
{"content_type": "text/plain", "payload": "Total actors: 10...", "is_error": false}
```

**CliHttpServerConfig:**

```cpp
struct CliHttpServerConfig {
  uint16_t    http_port         = 9090;
  std::string http_bind_address = "127.0.0.1";
  uint32_t    max_connections   = 100;
  std::string default_format    = "pretty";
  uint32_t    page_size         = 50;
};
```

### 5. Port Layout & Backward Compatibility

Three server actors, three independent port namespaces:

```
┌─────────────────────────────────────────────────────────────────┐
│                      Server Side                                 │
│                                                                  │
│  ┌──────────────────┐  ┌────────────────┐  ┌──────────────────┐ │
│  │ CliServerActor   │  │CliProtoServer  │  │CliHttpServer     │ │
│  │ (legacy)         │  │Actor           │  │Actor             │ │
│  │                  │  │                │  │                  │ │
│  │ UDS: /var/run/   │  │ UDS: /var/run/ │  │ TCP: 127.0.0.1   │ │
│  │   hpactor/       │  │   hpactor/     │  │   :9090          │ │
│  │   hpactor.sock   │  │   hpactor-cli. │  │                  │ │
│  │                  │  │   sock         │  │ Protocol:        │ │
│  │ TCP: 0 (off)     │  │                │  │ HTTP JSON        │ │
│  │                  │  │ TCP: 9091      │  │ (POST /cli)      │ │
│  │ Protocol:        │  │                │  │                  │ │
│  │ raw text         │  │ Protocol:      │  │ Reuses:          │ │
│  │ (\n / \0)        │  │ HPAC Frame +   │  │ HTTPGateway      │ │
│  │                  │  │ protobuf       │  │                  │ │
│  │ Transport:       │  │                │  │                  │ │
│  │ raw ::read/      │  │ Transport:     │  │                  │ │
│  │ ::write          │  │ TcpTransport/  │  │                  │ │
│  │                  │  │ WireFrameConn  │  │                  │ │
│  └──────────────────┘  └────────────────┘  └──────────────────┘ │
│                                                                  │
│  All three share one CliSession + CommandNode tree               │
│  via ICliCommandHost / ISystemCliHost / ILifecycleCliHost        │
└─────────────────────────────────────────────────────────────────┘
```

**Client connection matrix:**

| Client | Server | Transport | Wire Format |
|--------|--------|-----------|-------------|
| `CliLocalActor` (stdin) | (same process) | `deliver_local()` | Raw text via `TypedMessage` |
| `CliClientActor` (TCP, UDS) | `CliProtoServerActor` | `TcpTransport` / `WireFrameConnection` | HPAC Frame + protobuf |
| `curl` / any HTTP client | `CliHttpServerActor` | `HTTPGateway` | JSON |
| Legacy `hpactor-cli` binary | `CliServerActor` | Raw `::read()`/`::write()` | `\n` / `\0` text |

**Deprecation path:**

- **Phase 1 (this change):** Legacy `CliServerActor` active (deprecated).
  `CliProtoServerActor` + `CliHttpServerActor` active (new).
- **Phase 2 (next release):** Legacy `CliServerActor` disabled by default.
  Users opt in via explicit TOML config.
- **Phase 3 (future):** Remove legacy `CliServerActor` entirely.

### 6. Command Handler Migration

Commands in `src/cli/commands/*.cpp` currently check `ctx.cli_actor` and
`ctx.cli_server_actor` conditionally. After migration, they use the host
interfaces:

```cpp
// Before (current)
result<void> SystemStatsCommand::execute(CommandContext& ctx) const override {
  auto* sys = ctx.system;
  auto* cli = ctx.cli_actor;
  std::map<std::string, std::string> kv;
  kv["Total actors"] = std::to_string(sys->actor_count());
  if (cli) {
    kv["CLI enabled"] = cli->config().enabled ? "yes" : "no";
  }
  ctx.output->header("System Statistics");
  ctx.output->key_value(kv);
  return result<void>::make();
}

// After (target)
result<void> SystemStatsCommand::execute(CommandContext& ctx) const override {
  if (ctx.system_host) {
    // Works identically for local (CliActor/CliServerActor) and remote
    // (CliClientActor). Local renders from ActorSystem; remote sends
    // CliCommand over wire and writes the response payload.
    ctx.system_host->render_system_stats(*ctx.output);
    return result<void>::make();
  }
  // Fallback: direct ActorSystem access for environments without a host
  // (e.g. unit tests that don't wire up a full CLI host).
  // ... existing inline code ...
}
```

For the migration period, `CommandContext` retains the legacy `cli_actor` and
`cli_server_actor` pointers (populated from the host interfaces). Commands
migrate to the interface pointers incrementally. Once all commands are migrated,
the legacy pointers are removed.

## Testing

### New test files

| Test file | Tier | Coverage |
|-----------|------|----------|
| `test_cli_command_host` | unit | Mock host → command dispatch. Commands use `ctx.command_host`/`system_host`/`lifecycle_host`. Validates all three interfaces dispatch through commands correctly. |
| `test_cli_wire_protocol` | unit | `CliCommand`/`CliResponse` protobuf round-trip. Varint-length-prefix framing. Protojson mapping. Edge cases: empty params, large payloads, UTF-8 paths. |
| `test_cli_client_actor` | integration | `CliClientActor` connect/disconnect to `CliServerActor` over loopback TCP. Exec mode: send command, receive response, exit. Interactive mode: line processing via `LineEditor`. Reconnect on connection drop. HTTP JSON transport variant. |
| `test_cli_server_proto` | integration | `CliServerActor` protobuf listener: accept, decode `CliCommand`, execute via `CliSession`, encode `CliResponse`. HTTP endpoint: `POST /cli` with JSON, receive JSON response. Legacy port unchanged. |
| `test_cli_http_endpoint` | integration | HTTP `POST /cli` endpoint: valid JSON request → 200 with valid JSON response. Malformed JSON → 400. Wrong path → 200 with `is_error=true`. Content-Type validation. |

### Existing test preservation

All 267 existing CLI tests must pass unchanged. `CommandContext` gains new
fields but the legacy `cli_actor`/`cli_server_actor` pointers remain populated
during migration. No existing test behavior changes.

## Files changed

| Action | File | Description |
|--------|------|-------------|
| **NEW** | `include/hpactor/cli/cli_command_host.hpp` | `ICliCommandHost`, `ISystemCliHost`, `ILifecycleCliHost` |
| **NEW** | `include/hpactor/cli/cli_client_actor.hpp` | `CliClientActor` class |
| **NEW** | `include/hpactor/cli/cli_client_config.hpp` | `CliClientConfig` struct |
| **NEW** | `src/cli/cli_client_actor.cpp` | Implementation |
| **NEW** | `proto/hpactor/cli.proto` | `CliCommand`, `CliResponse` messages |
| **MODIFY** | `include/hpactor/cli/command_context.hpp` | Add `command_host`/`system_host`/`lifecycle_host`; keep legacy pointers |
| **MODIFY** | `include/hpactor/cli/cli_local_actor.hpp` | Implement three host interfaces |
| **MODIFY** | `include/hpactor/cli/cli_server_actor.hpp` | Remove proto/HTTP listener code; keep legacy raw text only |
| **MODIFY** | `include/hpactor/cli/cli_server_config.hpp` | Remove `proto_uds_path`, `proto_tcp_port`, `http_port` fields |
| **NEW** | `include/hpactor/cli/cli_proto_server_actor.hpp` | `CliProtoServerActor` — `TcpTransport::listen()`, `WireFrameConnection` |
| **NEW** | `include/hpactor/cli/cli_proto_server_config.hpp` | `CliProtoServerConfig` struct |
| **NEW** | `include/hpactor/cli/cli_http_server_actor.hpp` | `CliHttpServerActor` — `HTTPGateway::route(POST /cli)` |
| **NEW** | `include/hpactor/cli/cli_http_server_config.hpp` | `CliHttpServerConfig` struct |
| **NEW** | `src/cli/cli_proto_server_actor.cpp` | Implementation |
| **NEW** | `src/cli/cli_http_server_actor.cpp` | Implementation |
| **MODIFY** | `src/cli/cli_local_actor.cpp` | Wire host interfaces into CliSession |
| **MODIFY** | `src/cli/cli_server_actor.cpp` | Remove `on_proto_client_*`, HPAC frame handling, HTTP endpoint |
| **MODIFY** | `src/cli/cli_session.cpp` | Populate new `CommandContext` fields from session host pointers |
| **MODIFY** | `src/cli/cli_session.hpp` | Add `set_command_host()`/`set_system_host()`/`set_lifecycle_host()` methods |
| **MODIFY** | `src/cli/commands/*.cpp` (18 files) | Commands use interface pointers instead of `cli_actor`/`cli_server_actor` |
| **MODIFY** | `src/cli/commands/ask_commands.cpp` | Convert to `ICommand` subclass with `CommandRegistration` |
| **MODIFY** | `tools/hpactor-cli/main.cpp` | Replace ~350 lines → thin main: parse args, create `ActorSystem` + `CliClientActor` |
| **MODIFY** | `tools/hpactor-cli/CMakeLists.txt` | Add `cli.proto` to protobuf generation |
| **MODIFY** | `src/config/parsers/cli_server_config_parser.cpp` | New self-registering parser for `[system.cli_server]` proto/HTTP fields |
| **REMOVE** | `tools/hpactor-cli/main.cpp` | Socket helpers: `make_nonblocking`, `connect_uds_nonblock`, `connect_tcp_nonblock`, `await_connect`, `send_line_async`, `recv_response_async`, `FdGuard` (~350 lines) |

## Acceptance Criteria

1. `ICliCommandHost`, `ISystemCliHost`, and `ILifecycleCliHost` interfaces exist
   and are the sole dependency of `CommandContext` for actor/system/lifecycle
   operations (legacy pointers retained for migration only).
2. `CliActor`, `CliServerActor`, and `CliClientActor` all implement all three
   host interfaces.
3. `hpactor-cli` binary is a thin `main()` that creates an `ActorSystem` with a
   `CliClientActor` — no raw socket code remains.
4. Wire protocol uses protobuf `CliCommand`/`CliResponse` with varint-length
   prefix framing for binary transport.
5. HTTP endpoint `POST /cli` accepts JSON-encoded `CliCommand` and returns
   JSON-encoded `CliResponse`, using standard protobuf JSON mapping.
6. Legacy raw text protocol continues to work on its existing port
   (deprecated but functional), with a deprecation notice on each connection.
7. All 267 existing CLI tests pass without modification.
8. New tests cover: command host interface dispatch, client actor
   connect/disconnect/reconnect, wire protocol serialization round-trip,
   HTTP JSON endpoint.
9. `curl -X POST http://localhost:9090/cli -H "Content-Type: application/json" -d '{"path":"system/stats"}'`
   returns a valid JSON `CliResponse`.
