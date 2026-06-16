# CLI Server Decomposition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extract the proto listener from `CliServerActor` into `CliProtoServerActor` (reuses `TcpTransport::listen()` + `WireFrameConnection`), create `CliHttpServerActor` (reuses `HTTPGateway` for `POST /cli` JSON), and strip `CliServerActor` to legacy raw-text only.

**Architecture:** Three focused server actors sharing one `CliSession` + `CommandNode` tree via the host interfaces. `CliProtoServerActor` uses `TcpTransport::listen()` so accepted connections are auto-wrapped in `WireFrameConnection` — no manual HPAC Frame encode/decode. `CliHttpServerActor` uses `HTTPGateway` for HTTP accept/parse/response — any HTTP client can interact. `CliServerActor` keeps only the legacy raw-text handler.

**Tech Stack:** C++20, protobuf, existing HPActor framework (DaemonActor, TcpTransport, WireFrameConnection, HTTPGateway, CliSession, CommandRegistry)

---

## File Structure

| Action | File | Responsibility |
|--------|------|----------------|
| **NEW** | `include/hpactor/cli/cli_proto_server_actor.hpp` | `CliProtoServerActor` class |
| **NEW** | `include/hpactor/cli/cli_proto_server_config.hpp` | `CliProtoServerConfig` struct |
| **NEW** | `src/cli/cli_proto_server_actor.cpp` | Implementation |
| **NEW** | `include/hpactor/cli/cli_http_server_actor.hpp` | `CliHttpServerActor` class |
| **NEW** | `include/hpactor/cli/cli_http_server_config.hpp` | `CliHttpServerConfig` struct |
| **NEW** | `src/cli/cli_http_server_actor.cpp` | Implementation |
| **MODIFY** | `include/hpactor/cli/cli_server_actor.hpp` | Remove proto listener members and methods |
| **MODIFY** | `src/cli/cli_server_actor.cpp` | Remove `on_proto_client_*`, `execute_cli_command`, `dispatch_rpc`, HPAC Frame code |
| **MODIFY** | `include/hpactor/cli/cli_server_config.hpp` | Remove `proto_uds_path`, `proto_tcp_port`, `http_port` fields |
| **MODIFY** | `src/CMakeLists.txt` | Add new source files |
| **NEW** | `tests/unit/cli/test_cli_proto_server.cpp` | Proto server unit tests |
| **NEW** | `tests/unit/cli/test_cli_http_server.cpp` | HTTP server unit tests |

---

### Task 1: Create CliProtoServerConfig and CliHttpServerConfig

**Files:** Create `include/hpactor/cli/cli_proto_server_config.hpp`, `include/hpactor/cli/cli_http_server_config.hpp`

- [ ] **Step 1: Write CliProtoServerConfig**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor {
namespace cli {

/// \brief Configuration for the protobuf CLI server (CliProtoServerActor).
struct CliProtoServerConfig {
    /// \brief Unix domain socket path for protobuf CLI connections.
    std::string uds_listen_path;

    /// \brief TCP listen port. 0 means disabled.
    uint16_t tcp_listen_port = 0;

    /// \brief TCP bind address.
    std::string tcp_bind_address = "127.0.0.1";

    /// \brief Maximum concurrent sessions.
    uint32_t max_sessions = 16;

    /// \brief Session idle timeout (default 5 minutes).
    std::chrono::milliseconds session_timeout{300000};

    /// \brief Default output format: "pretty", "json", or "tabular".
    std::string default_format = "pretty";

    /// \brief Number of items per paged output page.
    uint32_t page_size = 50;

    /// \brief Permission mode for the UDS socket.
    uint32_t uds_socket_mode = 0660;

    /// \brief Owner user name for the UDS socket (optional).
    std::string uds_socket_owner;

    /// \brief Owner group name for the UDS socket (optional).
    std::string uds_socket_group;
};

} // namespace cli
} // namespace hpactor
```

- [ ] **Step 2: Write CliHttpServerConfig**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace hpactor {
namespace cli {

/// \brief Configuration for the HTTP JSON CLI server (CliHttpServerActor).
struct CliHttpServerConfig {
    /// \brief HTTP listen port for POST /cli JSON endpoint.
    uint16_t http_port = 9090;

    /// \brief HTTP bind address.
    std::string http_bind_address = "127.0.0.1";

    /// \brief Maximum concurrent HTTP connections.
    uint32_t max_connections = 100;

    /// \brief Default output format.
    std::string default_format = "pretty";

    /// \brief Number of items per paged output page.
    uint32_t page_size = 50;
};

} // namespace cli
} // namespace hpactor
```

- [ ] **Step 3: Verify headers compile**

```bash
ninja -C build hpactor_lib
```

Expected: compiles (headers are standalone, no references yet).

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cli/cli_proto_server_config.hpp include/hpactor/cli/cli_http_server_config.hpp
git commit -m "feat: add CliProtoServerConfig and CliHttpServerConfig

Configuration structs for the new protobuf and HTTP JSON CLI server actors.

Issue: #306"
```

---

### Task 2: Clean up CliServerConfig — remove proto/HTTP fields

**File:** Modify `include/hpactor/cli/cli_server_config.hpp`

- [ ] **Step 1: Remove proto/HTTP fields**

Remove these fields from `CliServerConfig`:
- `proto_uds_path`
- `proto_tcp_port`
- `http_port`
- `http_bind_address`
- `proto_uds_socket_mode`
- `proto_uds_socket_owner`
- `proto_uds_socket_group`

The struct should only contain legacy raw-text fields: `uds_listen_path`, `tcp_listen_port`, `tcp_bind_address`, `max_sessions`, `session_timeout`, `default_format`, `page_size`, `uds_socket_mode`, `uds_socket_owner`, `uds_socket_group`.

- [ ] **Step 2: Build and verify**

```bash
ninja -C build hpactor_lib
```

Expected: compiles. Any code referencing the removed fields will fail — fix stale references.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/cli/cli_server_config.hpp
git commit -m "refactor: remove proto/HTTP fields from CliServerConfig

These fields move to CliProtoServerConfig and CliHttpServerConfig.
CliServerConfig now only has legacy raw-text listener fields.

Issue: #306"
```

---

### Task 3: Create CliProtoServerActor

**Files:** Create `include/hpactor/cli/cli_proto_server_actor.hpp`, `src/cli/cli_proto_server_actor.cpp`

- [ ] **Step 1: Write the header**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli/cli_proto_server_config.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace hpactor {

class ActorSystem;

namespace net {
class EventLoop;
class TcpAcceptor;
class UnixDomainAcceptor;
class TcpTransport;
class Connection;
using ConnectionPtr = std::shared_ptr<Connection>;
} // namespace net

namespace cli {

class CliSession;
struct CommandNode;

/// \brief Protobuf CLI server operating as a daemon actor.
///
/// Uses \c net::TcpTransport::listen() for both UDS and TCP listeners.
/// Accepted connections are automatically wrapped in
/// \c net::WireFrameConnection instances which handle HPAC Frame
/// decode/encode transparently.  Each connection gets a \c CliSession
/// for transport-agnostic command dispatch.
///
/// Implements all three CLI host interfaces by querying the local
/// \c ActorSystem.
class CliProtoServerActor : public DaemonActor,
                            public ICliCommandHost,
                            public ISystemCliHost,
                            public ILifecycleCliHost {
  public:
    static constexpr const char* kActorTypeName = "CliProtoServerActor";

    CliProtoServerActor(ActorContext* ctx, ActorSystem& system,
                        const CliProtoServerConfig& config);
    ~CliProtoServerActor() override;

    // DaemonActor
    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;
    bool is_system_actor() const override { return true; }

    // ICliCommandHost
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

    // ISystemCliHost
    void render_system_stats(OutputFormatter& output) override;
    void render_memory_stats(OutputFormatter& output) override;
    void render_fault_status(OutputFormatter& output) override;
    void render_dlq_list(OutputFormatter& output, std::string_view filter = "") override;
    result<void> dlq_replay(uint32_t index, ActorId target) override;

    // ILifecycleCliHost
    result<void> drain() override;
    result<void> shutdown() override;

    void request_shutdown() { running_ = false; }

  private:
    void build_command_tree();
    void on_client_connected(net::ConnectionPtr conn);
    void on_frame_received(net::ConnectionPtr conn, adt::StreamBuffer data);

    /// Send an HPAC Frame containing @p data on @p conn.
    static void send_hpac_frame(net::ConnectionPtr conn, const std::string& data);

    ActorSystem& system_;
    CliProtoServerConfig config_;
    std::unique_ptr<net::EventLoop> loop_;
    std::unique_ptr<net::TcpTransport> transport_;
    std::unique_ptr<CommandNode> command_tree_;
    bool running_ = true;

    struct SessionState {
        std::unique_ptr<CliSession> session;
        std::chrono::steady_clock::time_point last_activity;
    };
    /// Map from Connection* → SessionState.
    std::unordered_map<net::ConnectionPtr, SessionState> sessions_;
};

} // namespace cli
} // namespace hpactor
```

- [ ] **Step 2: Write the implementation**

```cpp
#include <hpactor/cli.pb.h>
#include <hpactor/cli/cli_proto_server_actor.hpp>
#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/command_tree_builder.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/net/wireframe_connection.hpp>

#include <arpa/inet.h>
#include <cstring>

namespace hpactor {
namespace cli {

// ── Construction / destruction ──────────────────────────────────────

CliProtoServerActor::CliProtoServerActor(ActorContext* ctx, ActorSystem& system,
                                         const CliProtoServerConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      loop_(std::make_unique<net::EventLoop>()) {}

CliProtoServerActor::~CliProtoServerActor() = default;

// ── DaemonActor ─────────────────────────────────────────────────────

void CliProtoServerActor::on_daemon_start() {
    build_command_tree();

    net::TlsConfig tls{};
    net::PoolConfig pool{};
    pool.min_connections = 1;
    pool.max_connections = config_.max_sessions;

    // Use a loopback endpoint as the local endpoint for the transport.
    transport_ = std::make_unique<net::TcpTransport>(
        Ipv4Endpoint{0x7F000001, 0}, tls, pool);

    // Wire the actor message handler so accepted connections deliver
    // frames to on_frame_received.
    transport_->set_actor_message_handler(
        [this](const net::WireFrame& wf) {
            // This handler fires for each decoded HPAC Frame.
            // We need to route it to the correct session.
        });

    // Listen on UDS.
    if (!config_.uds_listen_path.empty()) {
        // TcpTransport doesn't have a UDS listen — we use UnixDomainAcceptor
        // directly for UDS, wrapping accepted fds in WireFrameConnection.
        // For simplicity, use TcpTransport::listen() for TCP only, and
        // handle UDS with a raw acceptor + manual WireFrameConnection creation.
    }

    // Listen on TCP via TcpTransport.
    if (config_.tcp_listen_port > 0) {
        transport_->listen(config_.tcp_listen_port);
    }
}

void CliProtoServerActor::on_daemon_stop() {
    running_ = false;
    sessions_.clear();
    transport_.reset();
    command_tree_.reset();
}

bool CliProtoServerActor::run_once() {
    if (!running_) return false;
    loop_->wait(100);
    return running_;
}

// ── Command tree ────────────────────────────────────────────────────

void CliProtoServerActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");
    build_command_tree_from_registry(*root);
    command_tree_ = std::move(root);
}

// ── HPAC Frame helpers ──────────────────────────────────────────────

void CliProtoServerActor::send_hpac_frame(net::ConnectionPtr conn,
                                           const std::string& data) {
    StreamBuffer frame;
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'C'};
    frame.append(magic.data(), 4);
    uint32_t payload_len = static_cast<uint32_t>(data.size());
    uint32_t net_len = htonl(payload_len);
    frame.append(reinterpret_cast<const uint8_t*>(&net_len), 4);
    frame.append(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    conn->send(frame);
}

// ── Client event handlers ───────────────────────────────────────────

void CliProtoServerActor::on_client_connected(net::ConnectionPtr conn) {
    if (sessions_.size() >= config_.max_sessions) {
        conn->close();
        return;
    }

    auto formatter = OutputFormatter::create(config_.default_format);
    auto session = std::make_unique<CliSession>(
        &system_, command_tree_.get(), std::move(formatter),
        [this, conn](const std::string& text) {
            send_hpac_frame(conn, text);
        },
        config_.page_size);
    session->set_command_host(this);
    session->set_system_host(this);
    session->set_lifecycle_host(this);

    SessionState state;
    state.session = std::move(session);
    state.last_activity = std::chrono::steady_clock::now();
    sessions_.emplace(conn, std::move(state));

    // Set the frame handler on the underlying WireFrameConnection
    // so decoded HPAC frames are delivered here.
    static_cast<net::WireFrameConnection*>(conn.get())
        ->set_frame_handler([this, conn](adt::StreamBuffer data) {
            on_frame_received(conn, std::move(data));
        });
}

void CliProtoServerActor::on_frame_received(net::ConnectionPtr conn,
                                             adt::StreamBuffer data) {
    auto it = sessions_.find(conn);
    if (it == sessions_.end()) return;

    CliCommand cmd;
    if (!cmd.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        CliResponse resp;
        resp.set_is_error(true);
        resp.set_error_code(1);
        std::string wire = resp.SerializeAsString();
        send_hpac_frame(conn, wire);
        return;
    }

    it->second.last_activity = std::chrono::steady_clock::now();

    if (!cmd.rpc_method().empty()) {
        // Structured RPC — dispatch directly.
        std::string reply_bytes = dispatch_rpc(cmd.rpc_method(), cmd.rpc_request());
        send_hpac_frame(conn, reply_bytes);
        return;
    }

    // Command-tree dispatch: reconstruct line and feed to CliSession.
    std::string line = "/" + cmd.path();
    for (const auto& arg : cmd.args()) line += " " + arg;
    for (const auto& [k, v] : cmd.params()) {
        line += " --" + k;
        if (!v.empty()) line += " " + v;
    }
    if (!cmd.format().empty()) line += " --format " + cmd.format();

    it->second.session->process_line(line);
}

// ── dispatch_rpc (copied from existing CliServerActor) ───────────────

std::string CliProtoServerActor::dispatch_rpc(
    const std::string& method, const std::string& request_bytes) {
    if (method == "inspect") {
        InspectStateRequest req;
        if (!req.ParseFromString(request_bytes)) return "";
        auto reply = inspect(ActorId{req.target_actor_id()}, req);
        return reply ? reply->SerializeAsString() : "";
    }
    if (method == "kill") {
        KillRequest req;
        if (!req.ParseFromString(request_bytes)) return "";
        auto reply = kill(ActorId{req.target_actor_id()}, req);
        return reply ? reply->SerializeAsString() : "";
    }
    if (method == "quarantine") {
        QuarantineRequest req;
        if (!req.ParseFromString(request_bytes)) return "";
        auto reply = quarantine(ActorId{req.target_actor_id()}, req);
        return reply ? reply->SerializeAsString() : "";
    }
    if (method == "enumerate") {
        auto actors = enumerate(request_bytes);
        ListActorsReply list_reply;
        for (auto& a : actors) {
            auto* pb = list_reply.add_actors();
            pb->set_actor_id(a.actor_id);
            pb->set_actor_type(a.actor_type);
            pb->set_state(a.state);
        }
        return list_reply.SerializeAsString();
    }
    return "";
}

// ── ICliCommandHost (same as CliServerActor::inspect/kill/quarantine/enumerate) ──
// Copy the implementations from cli_server_actor.cpp.
// ... (identical to existing CliServerActor implementations)

// ── ISystemCliHost (render_* — same as CliServerActor) ───────────────
// Copy the implementations from cli_server_actor.cpp.
// ... (identical to existing CliServerActor implementations)

// ── ILifecycleCliHost ───────────────────────────────────────────────
// Copy from cli_server_actor.cpp.
// ...

} // namespace cli
} // namespace hpactor
```

- [ ] **Step 3: Add to build system**

In `src/CMakeLists.txt`, add `cli/cli_proto_server_actor.cpp` to the CLI source list.

- [ ] **Step 4: Build and verify**

```bash
ninja -C build hpactor_lib
```

Expected: compiles. Fix any missing includes or type errors.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/cli/cli_proto_server_actor.hpp src/cli/cli_proto_server_actor.cpp src/CMakeLists.txt
git commit -m "feat: add CliProtoServerActor — protobuf CLI server

Uses TcpTransport::listen() so accepted connections are auto-wrapped
in WireFrameConnection with HPAC Frame decode/encode. Each connection
gets a CliSession for transport-agnostic command dispatch.

Issue: #306"
```

---

### Task 4: Extract proto listener code from CliServerActor

**Files:** Modify `include/hpactor/cli/cli_server_actor.hpp`, `src/cli/cli_server_actor.cpp`

- [ ] **Step 1: Remove proto listener members from header**

Remove from `CliServerActor`:
- `proto_sessions_` member
- `proto_uds_acceptor_` member
- `proto_tcp_acceptor_` member
- `on_proto_client_accepted()` method
- `on_proto_client_readable()` method
- `close_proto_session()` method
- `execute_cli_command()` method
- `dispatch_rpc()` method
- `ProtoSessionState` struct

Keep: legacy `sessions_`, `uds_acceptor_`, `tcp_acceptor_`, `on_client_accepted`, `on_client_readable`, `close_session`.

- [ ] **Step 2: Remove proto code from implementation**

Remove from `src/cli/cli_server_actor.cpp`:
- Proto acceptor setup in `on_daemon_start()` (the `proto_uds_acceptor_` and `proto_tcp_acceptor_` blocks)
- Proto session cleanup in `on_daemon_stop()`
- `on_proto_client_accepted()` implementation
- `on_proto_client_readable()` implementation (the ~150 lines of HPAC Frame decode + HTTP detection + dispatch)
- `close_proto_session()` implementation
- `execute_cli_command()` implementation
- `dispatch_rpc()` implementation
- `#include <hpactor/cli.pb.h>` (if no longer needed)
- `#include <hpactor/msg/frame.hpp>` (if no longer needed)

- [ ] **Step 3: Build and verify**

```bash
ninja -C build hpactor_lib
./build/tests/unit/cli/test_unit_cli 2>&1 | tail -3
```

Expected: 274 tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cli/cli_server_actor.hpp src/cli/cli_server_actor.cpp
git commit -m "refactor: extract proto listener from CliServerActor

Remove on_proto_client_*, execute_cli_command, dispatch_rpc, HPAC Frame
handling. CliServerActor now only handles legacy raw-text connections.
Proto/HTTP listeners moved to CliProtoServerActor and CliHttpServerActor.

Issue: #306"
```

---

### Task 5: Create CliHttpServerActor

**Files:** Create `include/hpactor/cli/cli_http_server_actor.hpp`, `src/cli/cli_http_server_actor.cpp`

- [ ] **Step 1: Write the header**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli/cli_http_server_config.hpp>

#include <memory>
#include <string>

namespace hpactor {

class ActorSystem;

namespace net {
class HTTPGateway;
class HTTPConnection;
} // namespace net

namespace cli {

class CliSession;
struct CommandNode;

/// \brief HTTP JSON CLI server operating as a daemon actor.
///
/// Reuses \c net::HTTPGateway for non-blocking listen/accept and HTTP
/// protocol handling.  Routes \c POST /cli with JSON-encoded
/// \c CliCommand body through \c CliSession and returns a
/// JSON-encoded \c CliResponse.
///
/// Implements \c ISystemCliHost and \c ILifecycleCliHost for
/// system queries and lifecycle operations over HTTP.
/// \c ICliCommandHost is NOT implemented — HTTP clients use
/// command-tree dispatch only (no structured RPC).
class CliHttpServerActor : public DaemonActor,
                           public ISystemCliHost,
                           public ILifecycleCliHost {
  public:
    static constexpr const char* kActorTypeName = "CliHttpServerActor";

    CliHttpServerActor(ActorContext* ctx, ActorSystem& system,
                       const CliHttpServerConfig& config);
    ~CliHttpServerActor() override;

    // DaemonActor
    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;
    bool is_system_actor() const override { return true; }

    // ISystemCliHost
    void render_system_stats(OutputFormatter& output) override;
    void render_memory_stats(OutputFormatter& output) override;
    void render_fault_status(OutputFormatter& output) override;
    void render_dlq_list(OutputFormatter& output, std::string_view filter = "") override;
    result<void> dlq_replay(uint32_t index, ActorId target) override;

    // ILifecycleCliHost
    result<void> drain() override;
    result<void> shutdown() override;

    void request_shutdown() { running_ = false; }

  private:
    void build_command_tree();
    void on_http_request(net::HTTPConnection* conn, class HttpRequest&& req);

    ActorSystem& system_;
    CliHttpServerConfig config_;
    std::unique_ptr<net::HTTPGateway> gateway_;
    std::unique_ptr<CommandNode> command_tree_;
    bool running_ = true;
};

} // namespace cli
} // namespace hpactor
```

- [ ] **Step 2: Write the implementation — on_http_request()**

```cpp
void CliHttpServerActor::on_http_request(net::HTTPConnection* conn,
                                          HttpRequest&& req) {
    if (req.method != HttpMethod::Post || req.path != "/cli") {
        // 404 for non-CLI routes.
        HttpResponse resp;
        resp.status_code = 404;
        resp.body = R"({"error":"not found"})";
        gateway_->send_response(conn, std::move(resp));
        return;
    }

    // Parse JSON body as CliCommand.
    CliCommand cmd;
    // Use protobuf JSON parsing (protojson or manual JSON→proto mapping).
    // For the initial implementation, accept a simple JSON mapping:
    //   {"path": "system/stats", "params": {...}, "args": [...], "format": "pretty"}
    //
    // This can use google::protobuf::util::JsonStringToMessage() or
    // a manual JSON parser for the four CliCommand fields.

    if (!google::protobuf::util::JsonStringToMessage(req.body, &cmd).ok()) {
        CliResponse resp;
        resp.set_is_error(true);
        resp.set_error_code(1);
        resp.set_content_type("application/json");
        std::string json_out;
        google::protobuf::util::MessageToJsonString(resp, &json_out);
        HttpResponse http_resp;
        http_resp.status_code = 400;
        http_resp.body = json_out;
        gateway_->send_response(conn, std::move(http_resp));
        return;
    }

    // Execute via CliSession.
    std::string captured;
    auto formatter = OutputFormatter::create(
        cmd.format().empty() ? config_.default_format : cmd.format());
    CliSession session(&system_, command_tree_.get(), std::move(formatter),
                       [&captured](const std::string& text) { captured = text; },
                       config_.page_size);
    session.set_system_host(this);
    session.set_lifecycle_host(this);

    // Reconstruct command line from CliCommand.
    std::string line = "/" + cmd.path();
    for (const auto& arg : cmd.args()) line += " " + arg;
    for (const auto& [k, v] : cmd.params()) {
        line += " --" + k;
        if (!v.empty()) line += " " + v;
    }
    if (!cmd.format().empty()) line += " --format " + cmd.format();

    session.process_line(line);

    CliResponse resp;
    resp.set_content_type("text/plain");
    resp.set_payload(captured);
    std::string json_out;
    google::protobuf::util::MessageToJsonString(resp, &json_out);

    HttpResponse http_resp;
    http_resp.status_code = 200;
    http_resp.content_type = "application/json";
    http_resp.body = json_out;
    gateway_->send_response(conn, std::move(http_resp));
}
```

- [ ] **Step 3: Add to build system**

In `src/CMakeLists.txt`, add `cli/cli_http_server_actor.cpp`.

- [ ] **Step 4: Build and verify**

```bash
ninja -C build hpactor_lib
./build/tests/unit/cli/test_unit_cli 2>&1 | tail -3
```

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/cli/cli_http_server_actor.hpp src/cli/cli_http_server_actor.cpp src/CMakeLists.txt
git commit -m "feat: add CliHttpServerActor — HTTP JSON CLI server

Reuses HTTPGateway for POST /cli JSON endpoint. Accepts JSON-encoded
CliCommand, routes through CliSession, returns JSON-encoded CliResponse.
Enables any HTTP client (curl, Python, browser) to interact with the CLI.

Issue: #306"
```

---

### Task 6: Write unit test for CliProtoServerActor

**File:** Create `tests/unit/cli/test_cli_proto_server.cpp`

- [ ] **Step 1: Write tests**

```cpp
#include <hpactor/cli/cli_proto_server_actor.hpp>
#include <hpactor/cli/cli_proto_server_config.hpp>
#include <hpactor/core/actor_system.hpp>
#include <gtest/gtest.h>

TEST(CliProtoServer, ConstructAndStart) {
    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0;
    hpactor::ActorSystem system(sys_config);

    hpactor::cli::CliProtoServerConfig cfg;
    cfg.tcp_listen_port = 19191;
    cfg.tcp_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliProtoServerActor>(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto* raw = static_cast<hpactor::cli::CliProtoServerActor*>(
        system.get_actor(server.id()).get());
    ASSERT_TRUE(raw != nullptr);
    ASSERT_TRUE(raw->is_system_actor());

    raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(CliProtoServer, ImplementsHostInterfaces) {
    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0;
    hpactor::ActorSystem system(sys_config);

    hpactor::cli::CliProtoServerConfig cfg;
    auto server = system.spawn<hpactor::cli::CliProtoServerActor>(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto* raw = static_cast<hpactor::cli::CliProtoServerActor*>(
        system.get_actor(server.id()).get());
    
    // Cast to host interfaces — verify inheritance.
    auto* cmd_host = static_cast<hpactor::cli::ICliCommandHost*>(raw);
    auto* sys_host = static_cast<hpactor::cli::ISystemCliHost*>(raw);
    auto* life_host = static_cast<hpactor::cli::ILifecycleCliHost*>(raw);
    ASSERT_TRUE(cmd_host != nullptr);
    ASSERT_TRUE(sys_host != nullptr);
    ASSERT_TRUE(life_host != nullptr);

    raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```

- [ ] **Step 2: Register in test CMakeLists and run**

```bash
ninja -C build tests/unit/cli/test_unit_cli
./build/tests/unit/cli/test_unit_cli --gtest_filter="CliProtoServer*"
```

- [ ] **Step 3: Commit**

---

### Task 7: Write unit test for CliHttpServerActor

**File:** Create `tests/unit/cli/test_cli_http_server.cpp`

- [ ] **Step 1: Write tests** (similar structure — construct, start, verify interface inheritance, shutdown)

- [ ] **Step 2: Register and run**

- [ ] **Step 3: Commit**

---

### Task 8: Full verification

- [ ] **Step 1: Full build**

```bash
ninja -C build
```

- [ ] **Step 2: Full test suite**

```bash
./build/tests/unit/cli/test_unit_cli 2>&1 | tail -3
```

Expected: all 274+ CLI tests pass.

- [ ] **Step 3: Commit final adjustments**

---

## Acceptance Criteria

1. `CliServerActor` handles only legacy raw-text connections — no proto/HTTP code remains
2. `CliProtoServerActor` uses `TcpTransport::listen()` for accepting, `WireFrameConnection` for HPAC Frame decode/encode
3. `CliHttpServerActor` uses `HTTPGateway` for `POST /cli` JSON endpoint
4. All three server actors implement the same host interfaces
5. All existing CLI tests pass without modification
6. New tests cover proto server and HTTP server construction + interface inheritance

## Deferred

- Full end-to-end integration tests (cli client → proto server, curl → HTTP server)
- `dispatch_rpc()` extracted to shared helper (currently duplicated between CliServerActor and CliProtoServerActor)
- UDS listen support in CliProtoServerActor (TcpTransport doesn't have UDS listen — needs manual acceptor + WireFrameConnection wrapping)
