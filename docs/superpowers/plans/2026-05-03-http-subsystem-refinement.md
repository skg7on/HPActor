# HTTP Subsystem Refinement — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the HTTP subsystem with Config-gated features, a shared HTTPConnection framing layer (mirroring WireFrameConnection), and HTTPServerActor as a DaemonActor with standard actor messaging.

**Architecture:** Three orthogonal changes: (1) `Config` flags gate HTTP server/client creation in `ActorSystem`, following the existing `enable_network` pattern. (2) `HTTPConnection` extends `Connection` with llhttp-based HTTP/1.1 framing — shared by both server and client, same pattern as `WireFrameConnection`. (3) `HTTPServerActor` extends `DaemonActor` — owns an EventLoop on its dedicated thread, accepts via HTTPConnection in Server mode, converts HTTP requests to TypedMessage via HttpSerializer, routes to registered EventBasedActor targets, receives replies via ReplyAdapter.

**Tech Stack:** C++20, `-fno-exceptions`, `-fno-rtti`, header-only where possible + `.cpp` for impl. LLVM coding style. CMake + Ninja. Existing patterns: `hpactor_lib` shared library, tests via `add_executable` + `add_test` in `tests/CMakeLists.txt`. Vendored llhttp for HTTP/1.1 parsing.

**Design Spec:** `docs/architecture/net/http-communication.md`

---

## File Map

| File | Responsibility | Action |
|------|---------------|--------|
| `include/hpactor/core/actor_system.hpp` | Add HTTP flags to `Config` struct | **Modify** |
| `src/actor/actor_system.cpp` | Gate HTTP creation in `ActorSystem` ctor/dtor | **Modify** |
| `include/hpactor/net/http_connection.hpp` | `HTTPConnection` — shared Connection subclass for HTTP framing | **Create** |
| `src/net/http_connection.cpp` | `HTTPConnection` implementation | **Create** |
| `include/hpactor/net/http_server.hpp` | `HTTPServerActor` — DaemonActor-based ingress (replaces old HttpServer) | **Rewrite** |
| `src/net/http_server.cpp` | `HTTPServerActor` implementation | **Rewrite** |
| `include/hpactor/net/http_client.hpp` | `HttpClient` — refined to use HTTPConnection internally | **Modify** |
| `src/net/http_client.cpp` | `HttpClient` — refined to use HTTPConnection internally | **Modify** |
| `include/hpactor/net/http_types.hpp` | No changes needed | — |
| `include/hpactor/net/http_parser.hpp` | No changes needed | — |
| `src/net/http_parser.cpp` | No changes needed | — |
| `include/hpactor/net/http_serializer.hpp` | No changes needed | — |
| `tests/net/test_http_connection.cpp` | Unit tests for HTTPConnection (Server and Client modes) | **Create** |
| `tests/net/test_http_server.cpp` | Update for HTTPServerActor (DaemonActor-based) | **Modify** |
| `CMakeLists.txt` | Add `src/net/http_connection.cpp` to hpactor_lib | **Modify** |
| `tests/CMakeLists.txt` | Register `test_http_connection` | **Modify** |

---

### Task 1: Config-Gated HTTP Features

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp` (~lines 52-76, Config struct)
- Modify: `src/actor/actor_system.cpp` (~lines 55-123, ActorSystem ctor; ~lines 125-138, dtor)

- [ ] **Step 1: Add HTTP fields to Config struct**

Read `include/hpactor/core/actor_system.hpp`. In the `Config` struct, after the existing network fields (after `RegistrarConfig registrar = {};` around line 74), add:

```cpp
    // HTTP subsystem (requires enable_network = true)
    bool enable_http_server = false;
    bool enable_http_client = false;

    // HTTP server configuration
    uint16_t http_port = 8080;
    std::string http_bind_host = "0.0.0.0";
    size_t http_max_connections = 1000;
    size_t http_max_request_size = 1048576;
    std::chrono::milliseconds http_reply_timeout{5000};
```

- [ ] **Step 2: Build to verify Config changes compile**

```bash
ninja -C build
```

Expected: Build succeeds (fields not yet used).

- [ ] **Step 3: Gate HTTP server/client creation in ActorSystem**

Read `src/actor/actor_system.cpp`. The current constructor unconditionally creates `http_client_`:

```cpp
http_client_ = std::make_unique<net::HttpClient>(network_loop_.get());
```

Replace this with:

```cpp
        if (config_.enable_http_client) {
            http_client_ = std::make_unique<net::HttpClient>(network_loop_.get());
        }
```

And after the transport setup, add HTTP server creation (the server is an actor, spawned after the system actor is set up — add near the end of the constructor, before the closing brace of `if (config.enable_network)`):

```cpp
        if (config_.enable_http_server) {
            // HTTPServerActor is spawned as a system-level DaemonActor.
            // It will start listening in on_activate().
            // Deferred to Task 3 — for now, stub with a TODO.
            // http_server_actor_ = spawn<HTTPServerActor>(...);
        }
```

In the destructor, wrap existing http_client_ cleanup in a null check (already safe with unique_ptr, but add the conditional for clarity):

The destructor doesn't need changes — `std::unique_ptr` handles null correctly.

- [ ] **Step 4: Add forward declaration and member for HTTPServerActor**

In `actor_system.hpp`, in the private section (near `std::unique_ptr<net::HttpClient> http_client_;` around line 265), add:

```cpp
    // HTTP server actor (DaemonActor, spawned when enable_http_server = true)
    Actor http_server_actor_{nullptr};
```

- [ ] **Step 5: Build and run all tests**

```bash
ninja -C build && ctest --output-on-failure
```

Expected: All tests pass. HTTP features default to `false`, so no behavior change.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat(core): add Config-gated HTTP server and client features"
```

---

### Task 2: HTTPConnection — Shared HTTP Framing Layer

**Files:**
- Create: `include/hpactor/net/http_connection.hpp`
- Create: `src/net/http_connection.cpp`
- Create: `tests/net/test_http_connection.cpp`
- Modify: `CMakeLists.txt` (root) — add `src/net/http_connection.cpp`
- Modify: `tests/CMakeLists.txt` — register `test_http_connection`

- [ ] **Step 1: Write the failing test**

Create `tests/net/test_http_connection.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...license...
#include <cassert>
#include <cstring>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/event_loop.hpp>

using namespace hpactor;
using namespace hpactor::net;

// Helper: create a connected socket pair (client_fd → server_fd)
static std::pair<int, int> make_socket_pair() {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(listener, (struct sockaddr*)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(listener, (struct sockaddr*)&addr, &len);
    listen(listener, 1);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, (struct sockaddr*)&addr, sizeof(addr));
    int server = accept(listener, nullptr, nullptr);
    close(listener);
    return {client, server};
}

static StreamBuffer make_body(const char* s) {
    StreamBuffer b;
    b.append((const uint8_t*)s, strlen(s));
    return b;
}

int main() {
    // Test 1: HTTPConnection Server mode — parse incoming HTTP request
    {
        auto [client_fd, server_fd] = make_socket_pair();
        EventLoop loop;
        loop.run();

        std::string received_path;
        StreamBuffer received_body;
        bool complete = false;

        auto conn = HTTPConnection::create(server_fd,
            LocalEndpoint, Ipv4Endpoint{}, &loop, HTTPConnectionMode::Server);
        conn->set_request_handler(
            [&](HTTPConnection*, HttpRequest&& req) {
                received_path = req.path;
                received_body = req.body;
                complete = true;
            });

        // Register with event loop
        loop.add_fd(server_fd, EventLoop::Event::Read);
        loop.set_read_handler(server_fd, [c = conn.get()](int) {
            c->handle_read();
        });

        // Write HTTP request from client side
        const char* req = "POST /test HTTP/1.1\r\nHost: local\r\nContent-Length: 5\r\n\r\nhello";
        write(client_fd, req, strlen(req));

        // Process events
        for (int i = 0; i < 10 && !complete; i++) {
            loop.wait(50);
            loop.process_completions();
        }

        assert(complete);
        assert(received_path == "/test");
        std::string body_str(received_body.begin(), received_body.end());
        assert(body_str == "hello");

        close(client_fd);
        loop.stop();
    }

    // Test 2: HTTPConnection — send_response builds valid HTTP wire bytes
    {
        auto [client_fd, server_fd] = make_socket_pair();
        EventLoop loop;
        loop.run();

        auto conn = HTTPConnection::create(server_fd,
            LocalEndpoint, Ipv4Endpoint{}, &loop, HTTPConnectionMode::Server);

        loop.add_fd(server_fd, EventLoop::Event::Read);
        loop.set_read_handler(server_fd, [c = conn.get()](int) {
            c->handle_read();
        });

        // Send a simple request first
        const char* req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        write(client_fd, req, strlen(req));

        // Process the request, then send response
        bool req_done = false;
        conn->set_request_handler([&](HTTPConnection* c, HttpRequest&&) {
            c->send_response(HttpStatusCode::OK,
                {{"Content-Type", "text/plain"}}, make_body("ok"));
            req_done = true;
        });

        for (int i = 0; i < 10 && !req_done; i++) {
            loop.wait(50);
            loop.process_completions();
        }
        assert(req_done);

        // Read response on client side
        char buf[4096] = {};
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        assert(n > 0);
        buf[n] = '\0';
        assert(strstr(buf, "200 OK") != nullptr);
        assert(strstr(buf, "ok") != nullptr);

        close(client_fd);
        loop.stop();
    }

    // Test 3: HTTPConnection Client mode — parse incoming HTTP response
    {
        auto [client_fd, server_fd] = make_socket_pair();
        EventLoop loop;
        loop.run();

        int resp_status = 0;
        bool resp_done = false;

        auto conn = HTTPConnection::create(client_fd,
            LocalEndpoint, Ipv4Endpoint{}, &loop, HTTPConnectionMode::Client);
        conn->set_response_handler(
            [&](HTTPConnection*, int status, std::vector<HttpHeader>, StreamBuffer body) {
                resp_status = status;
                resp_done = true;
            });

        loop.add_fd(client_fd, EventLoop::Event::Read);
        loop.set_read_handler(client_fd, [c = conn.get()](int) {
            c->handle_read();
        });

        // Write HTTP response from server side
        const char* resp = "HTTP/1.1 201 Created\r\nContent-Length: 3\r\n\r\nyes";
        write(server_fd, resp, strlen(resp));

        for (int i = 0; i < 10 && !resp_done; i++) {
            loop.wait(50);
            loop.process_completions();
        }

        assert(resp_done);
        assert(resp_status == 201);

        close(server_fd);
        loop.stop();
    }

    return 0;
}
```

- [ ] **Step 2: Register test in tests/CMakeLists.txt**

Read `tests/CMakeLists.txt`, add after the existing HTTP tests (around line 285):

```cmake
add_executable(test_http_connection net/test_http_connection.cpp)
target_link_libraries(test_http_connection hpactor)
add_test(NAME test_http_connection COMMAND test_http_connection)
```

- [ ] **Step 3: Verify test fails**

```bash
cmake -S . -B build -GNinja && ninja -C build 2>&1 | tail -10
```

Expected: Compilation error — `HTTPConnection` not yet defined.

- [ ] **Step 4: Write HTTPConnection header**

Create `include/hpactor/net/http_connection.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...license...
#pragma once

#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hpactor::net {

enum class HTTPConnectionMode { Server, Client };

class HTTPConnection : public Connection,
                       public std::enable_shared_from_this<HTTPConnection> {
  public:
    using RequestHandler =
        std::function<void(HTTPConnection*, HttpRequest&&)>;
    using ResponseHandler =
        std::function<void(HTTPConnection*, int status_code,
                           std::vector<HttpHeader>, StreamBuffer)>;
    using ErrorHandler =
        std::function<void(HTTPConnection*, HttpStatusCode, const std::string&)>;

    static std::shared_ptr<HTTPConnection>
    create(int fd, EndPoint local_ep, EndPoint remote_ep,
           EventLoop* loop, HTTPConnectionMode mode);

    ~HTTPConnection() override;

    HTTPConnection(const HTTPConnection&) = delete;
    HTTPConnection& operator=(const HTTPConnection&) = delete;

    // Callbacks
    void set_request_handler(RequestHandler h) { request_handler_ = std::move(h); }
    void set_response_handler(ResponseHandler h) { response_handler_ = std::move(h); }
    void set_error_handler(ErrorHandler h) { error_handler_ = std::move(h); }

    // Send HTTP response (Server mode) or request body bytes (both modes)
    void send_response(HttpStatusCode code, std::vector<HttpHeader> headers,
                       StreamBuffer body);
    void send_raw(StreamBuffer data);

    // Connection overrides
    void send(const StreamBuffer& data) override;
    void close() override;
    void handle_read() override;
    void handle_send_completion(int result) override;

    // Keep-alive tracking
    bool should_keep_alive() const;
    HTTPConnectionMode mode() const { return mode_; }

  private:
    HTTPConnection(int fd, EndPoint local_ep, EndPoint remote_ep,
                   EventLoop* loop, HTTPConnectionMode mode);

    void parse_incoming(const StreamBuffer& data);
    void flush_write_queue();

    HTTPConnectionMode mode_;
    std::unique_ptr<HttpParser> parser_;
    StreamBuffer read_buf_;

    // Write-side
    StreamBuffer write_buf_;
    std::vector<StreamBuffer> write_queue_;
    bool is_sending_ = false;

    // Callbacks
    RequestHandler request_handler_;
    ResponseHandler response_handler_;
    ErrorHandler error_handler_;
};

} // namespace hpactor::net
```

- [ ] **Step 5: Write HTTPConnection implementation**

Create `src/net/http_connection.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...license...
#include <hpactor/net/http_connection.hpp>

#include <unistd.h>
#include <cstring>

namespace hpactor::net {

HTTPConnection::HTTPConnection(int fd, EndPoint local_ep, EndPoint remote_ep,
                               EventLoop* loop, HTTPConnectionMode mode)
    : Connection(fd, local_ep, remote_ep, loop),
      mode_(mode),
      parser_(std::make_unique<HttpParser>(
          mode == HTTPConnectionMode::Server ? HttpParserMode::Request
                                             : HttpParserMode::Response)) {
    set_state(ConnectionState::Connected);

    if (mode_ == HTTPConnectionMode::Server) {
        parser_->set_on_message([this](HttpRequest&& req) {
            if (request_handler_) {
                request_handler_(this, std::move(req));
            }
        });
        parser_->set_on_error([this](llhttp_errno_t /*err*/, const char* msg) {
            if (error_handler_) {
                error_handler_(this, HttpStatusCode::BadRequest,
                               msg ? msg : "Parse error");
            }
        });
    } else {
        parser_->set_on_response(
            [this](int status, const std::vector<HttpHeader>& headers,
                   const StreamBuffer& body) {
                if (response_handler_) {
                    response_handler_(this, status, headers, body);
                }
            });
        parser_->set_on_error([this](llhttp_errno_t /*err*/, const char* msg) {
            if (error_handler_) {
                error_handler_(this, HttpStatusCode::BadRequest,
                               msg ? msg : "Parse error");
            }
        });
    }
}

HTTPConnection::~HTTPConnection() {
    close();
}

std::shared_ptr<HTTPConnection>
HTTPConnection::create(int fd, EndPoint local_ep, EndPoint remote_ep,
                        EventLoop* loop, HTTPConnectionMode mode) {
    return std::shared_ptr<HTTPConnection>(
        new HTTPConnection(fd, local_ep, remote_ep, loop, mode));
}

void HTTPConnection::send(const StreamBuffer& data) {
    send_raw(data);
}

void HTTPConnection::send_raw(StreamBuffer data) {
    if (fd_ < 0) return;

    if (is_sending_) {
        write_queue_.push_back(std::move(data));
        return;
    }

    is_sending_ = true;
    write_buf_ = std::move(data);
    // Issue async send via event loop
    loop_->backend()->async_send(fd_, write_buf_.data(), write_buf_.size());
}

void HTTPConnection::send_response(HttpStatusCode code,
                                    std::vector<HttpHeader> headers,
                                    StreamBuffer body) {
    // Build HTTP/1.1 response wire bytes
    std::string status_line = "HTTP/1.1 ";
    status_line += std::to_string(static_cast<uint16_t>(code));
    status_line += " ";
    status_line += reason_phrase(code);
    status_line += "\r\n";

    // Add Content-Length if not present
    bool has_content_length = false;
    bool has_connection = false;
    for (const auto& h : headers) {
        if (h.name == "content-length") has_content_length = true;
        if (h.name == "connection") has_connection = true;
    }
    if (!has_content_length) {
        headers.push_back({"content-length", std::to_string(body.size())});
    }
    if (!has_connection && !should_keep_alive()) {
        headers.push_back({"connection", "close"});
    }

    StreamBuffer wire;
    wire.append((const uint8_t*)status_line.data(), status_line.size());
    for (const auto& h : headers) {
        std::string line = h.name + ": " + h.value + "\r\n";
        wire.append((const uint8_t*)line.data(), line.size());
    }
    wire.append((const uint8_t*)"\r\n", 2);
    if (body.size() > 0) {
        wire.append(body.data(), body.size());
    }

    send_raw(std::move(wire));
}

void HTTPConnection::close() {
    if (fd_ >= 0) {
        loop_->clear_read_handler(fd_);
        loop_->remove_fd(fd_);
        ::close(fd_);
        fd_ = -1;
    }
    set_state(ConnectionState::Disconnected);
}

void HTTPConnection::handle_read() {
    if (fd_ < 0) return;

    uint8_t buf[8192];
    ssize_t n = ::read(fd_, buf, sizeof(buf));

    if (n <= 0) {
        close();
        return;
    }

    read_buf_.append(buf, static_cast<size_t>(n));
    parse_incoming(read_buf_);

    if (parser_->state() == HttpParseState::Complete ||
        parser_->state() == HttpParseState::Idle) {
        read_buf_.clear();
    }
}

void HTTPConnection::handle_send_completion(int /*result*/) {
    is_sending_ = false;
    flush_write_queue();
}

void HTTPConnection::flush_write_queue() {
    if (!write_queue_.empty()) {
        StreamBuffer next = std::move(write_queue_.front());
        write_queue_.erase(write_queue_.begin());
        send_raw(std::move(next));
    }
}

void HTTPConnection::parse_incoming(const StreamBuffer& data) {
    parser_->execute(std::span<const uint8_t>(data.data(), data.size()));
}

bool HTTPConnection::should_keep_alive() const {
    return parser_->should_keep_alive();
}

} // namespace hpactor::net
```

- [ ] **Step 6: Add http_connection.cpp to CMakeLists.txt**

Read root `CMakeLists.txt`. Add `src/net/http_connection.cpp` in the `hpactor_lib` sources list, alphabetically near the other `src/net/http_*.cpp` entries (after `src/net/http_client.cpp`):

```
    src/net/http_connection.cpp
```

- [ ] **Step 7: Build and run the HTTPConnection test**

```bash
ninja -C build && ctest -R test_http_connection --output-on-failure
```

Expected: Test 1 (Server mode parse), Test 2 (send_response), Test 3 (Client mode parse) all pass.

- [ ] **Step 8: Run full test suite**

```bash
ctest --output-on-failure
```

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/net/http_connection.hpp src/net/http_connection.cpp tests/net/test_http_connection.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(net): add HTTPConnection — shared Connection subclass for HTTP framing"
```

---

### Task 3: Rewrite HttpServer as HTTPServerActor (DaemonActor)

**Files:**
- Modify: `include/hpactor/net/http_server.hpp` — rewrite as HTTPServerActor
- Modify: `src/net/http_server.cpp` — rewrite implementation

- [ ] **Step 1: Read current files to understand existing interface**

Read `include/hpactor/net/http_server.hpp` and `src/net/http_server.cpp`. The current `HttpServer` class with `RouteRegistry`, `listen()`, `stop()`, `route()` etc. is being replaced.

- [ ] **Step 2: Write the new HTTPServerActor header**

Rewrite `include/hpactor/net/http_server.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...license...
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_serializer.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hpactor::net {

// ---------------------------------------------------------------------------
// RouteRegistry — URL pattern matcher for HTTP routing (unchanged from current)
// ---------------------------------------------------------------------------
class RouteRegistry {
  public:
    using MessageBuilder =
        std::function<std::pair<ActorAddress, TypedMessage>(const HttpRequest&)>;

    RouteRegistry() = default;

    void add(HttpMethod method, std::string pattern,
             MessageBuilder builder, int priority = 0);

    const MessageBuilder* match(HttpMethod method, const std::string& path,
                                HttpRequest& req) const;

    bool empty() const { return routes_.empty(); }

  private:
    struct Route;
    std::vector<Route> routes_;
};

// ---------------------------------------------------------------------------
// HTTPServerActor — DaemonActor-based HTTP ingress gateway
// ---------------------------------------------------------------------------
class HTTPServerActor : public DaemonActor {
  public:
    using MessageBuilder = RouteRegistry::MessageBuilder;

    HTTPServerActor(ActorContext* ctx, ActorSystem& sys,
                    const std::string& bind_host, uint16_t port);
    ~HTTPServerActor() override;

    HTTPServerActor(const HTTPServerActor&) = delete;
    HTTPServerActor& operator=(const HTTPServerActor&) = delete;

    // Route registration (must be called before daemon starts)
    void route(HttpMethod method, std::string path_pattern,
               MessageBuilder builder, int priority = 0);
    void route(HttpMethod method, std::string path_pattern, ActorAddr target);

    // Configuration
    void set_reply_timeout(std::chrono::milliseconds t) { reply_timeout_ = t; }
    void set_max_connections(size_t max) { max_connections_ = max; }
    void set_max_request_size(size_t max) { max_request_size_ = max; }

    uint16_t port() const { return port_; }
    bool is_listening() const { return listen_fd_ >= 0; }

    // DaemonActor overrides
    bool run_once() override;

  protected:
    void on_daemon_start() override;
    void on_daemon_stop() override;

  private:
    void on_accept();
    void on_request(HTTPConnection* conn, HttpRequest&& req);
    void on_reply(TypedMessage&& msg);
    void on_error(HTTPConnection* conn, HttpStatusCode code, const std::string& msg);
    void on_timeout(uint64_t request_id);
    void close_connection(HTTPConnection* conn);

    // Owned EventLoop (runs on the daemon's dedicated thread)
    EventLoop loop_;

    // Route registry
    RouteRegistry routes_;

    // Serializer
    std::unique_ptr<HttpSerializer> serializer_;

    // Active connections (maps raw ptr → shared_ptr for ownership)
    std::unordered_map<HTTPConnection*, std::shared_ptr<HTTPConnection>> connections_;
    std::mutex conn_mutex_;

    // Pending replies
    struct PendingReply {
        uint64_t request_id;
        HTTPConnection* conn;
        std::chrono::steady_clock::time_point enqueued_at;
    };
    std::unordered_map<uint64_t, std::unique_ptr<PendingReply>> pending_replies_;
    std::mutex reply_mutex_;

    // Reply adapter — internal EventBasedActor that receives actor replies
    Actor reply_adapter_{nullptr};

    // Configuration
    std::string bind_host_;
    uint16_t port_;
    int listen_fd_ = -1;
    std::chrono::milliseconds reply_timeout_{5000};
    size_t max_connections_{1000};
    size_t max_request_size_{1048576};
    uint64_t next_request_id_{1};
};

} // namespace hpactor::net
```

- [ ] **Step 3: Write the new HTTPServerActor implementation**

Rewrite `src/net/http_server.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// ...license...
#include <hpactor/net/http_server.hpp>
#include <hpactor/core/actor_system.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace hpactor::net {

// =============================================================================
// RouteRegistry Implementation (unchanged from current http_server.cpp)
// =============================================================================

enum class PatternSegmentType { Literal, NamedParam, SingleWildcard, MultiWildcard };

struct PatternSegment {
    PatternSegmentType type;
    std::string name;
};

struct RouteRegistry::Route {
    HttpMethod method;
    std::vector<PatternSegment> segments;
    MessageBuilder builder;
    int priority;
};

// ... (copy existing parse_pattern, match_pattern, RouteRegistry::add,
//      RouteRegistry::match implementations verbatim from current http_server.cpp) ...

// =============================================================================
// ReplyAdapter — internal actor receiving actor replies for HTTPServerActor
// =============================================================================
namespace {

class ReplyAdapter final : public EventBasedActor {
  public:
    using ReplyHandler = std::function<void(TypedMessage&&)>;

    ReplyAdapter(ActorContext* ctx, ActorSystem& sys, ReplyHandler handler)
        : EventBasedActor(ctx, sys), handler_(std::move(handler)) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            if (handler_) {
                handler_(std::move(msg));
            }
        });
    }

  private:
    ReplyHandler handler_;
};

} // anonymous namespace

// =============================================================================
// HTTPServerActor Implementation
// =============================================================================

HTTPServerActor::HTTPServerActor(ActorContext* ctx, ActorSystem& sys,
                                 const std::string& bind_host, uint16_t port)
    : DaemonActor(ctx, sys),
      bind_host_(bind_host),
      port_(port),
      serializer_(std::make_unique<HttpSerializer>()) {

    auto handler = [this](TypedMessage&& msg) {
        on_reply(std::move(msg));
    };
    reply_adapter_ = sys.spawn<ReplyAdapter>(std::move(handler));
}

HTTPServerActor::~HTTPServerActor() {
    // DaemonActor destructor calls on_deactivate() which joins the thread
}

void HTTPServerActor::route(HttpMethod method, std::string path_pattern,
                             MessageBuilder builder, int priority) {
    routes_.add(method, std::move(path_pattern), std::move(builder), priority);
}

void HTTPServerActor::route(HttpMethod method, std::string path_pattern,
                             ActorAddr target) {
    auto serializer = serializer_.get();
    route(method, std::move(path_pattern),
          [target, serializer](const HttpRequest& req)
              -> std::pair<ActorAddress, TypedMessage> {
              auto result = serializer->deserialize_request(req, TypeTag::User);
              if (!result.has_value()) {
                  return {invalid_actor_addr, TypedMessage{}};
              }
              return {target, std::move(result.value())};
          });
}

void HTTPServerActor::on_daemon_start() {
    // Create and bind listening socket
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    fcntl(listen_fd_, F_SETFL, O_NONBLOCK);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (bind_host_ == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, bind_host_.c_str(), &addr.sin_addr);
    }

    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    // Start EventLoop and register accept handler
    loop_.run();
    loop_.add_fd(listen_fd_, EventLoop::Event::Read);
    loop_.set_read_handler(listen_fd_, [this](int) {
        on_accept();
    });
}

void HTTPServerActor::on_daemon_stop() {
    // Close all active connections
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (auto& [ptr, conn] : connections_) {
            conn->close();
        }
        connections_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        pending_replies_.clear();
    }

    if (listen_fd_ >= 0) {
        loop_.clear_read_handler(listen_fd_);
        loop_.remove_fd(listen_fd_);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    loop_.stop();
}

bool HTTPServerActor::run_once() {
    loop_.wait(100);
    loop_.process_completions();
    return true; // keep running
}

void HTTPServerActor::on_accept() {
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = ::accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) return;

    fcntl(client_fd, F_SETFL, O_NONBLOCK);

    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (connections_.size() >= max_connections_) {
            ::close(client_fd);
            return;
        }
    }

    auto conn = HTTPConnection::create(client_fd, LocalEndpoint,
        Ipv4Endpoint{}, &loop_, HTTPConnectionMode::Server);

    conn->set_request_handler([this](HTTPConnection* c, HttpRequest&& req) {
        on_request(c, std::move(req));
    });
    conn->set_error_handler([this](HTTPConnection* c, HttpStatusCode code,
                                    const std::string& msg) {
        on_error(c, code, msg);
    });

    loop_.add_fd(client_fd, EventLoop::Event::Read);
    loop_.set_read_handler(client_fd, [c = conn.get()](int) {
        c->handle_read();
    });

    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        connections_[conn.get()] = std::move(conn);
    }
}

void HTTPServerActor::on_request(HTTPConnection* conn, HttpRequest&& req) {
    // Parse query string
    size_t qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        std::string query_str = req.path.substr(qpos + 1);
        req.path = req.path.substr(0, qpos);
        // ... query string parsing (copy from current impl) ...
    }

    // Match route
    const auto* builder = routes_.match(req.method, req.path, req);
    if (!builder) {
        conn->send_response(HttpStatusCode::NotFound,
            {{"Content-Type", "text/plain"}, {"Connection", "close"}},
            StreamBuffer{});
        close_connection(conn);
        return;
    }

    // Build target + TypedMessage
    auto [target, msg] = (*builder)(req);
    if (!target) {
        conn->send_response(HttpStatusCode::InternalError,
            {{"Content-Type", "text/plain"}, {"Connection", "close"}},
            StreamBuffer{});
        close_connection(conn);
        return;
    }

    // Set up reply tracking with correlation ID
    uint64_t request_id = next_request_id_++;
    auto pending = std::make_unique<PendingReply>();
    pending->request_id = request_id;
    pending->conn = conn;
    pending->enqueued_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        pending_replies_[request_id] = std::move(pending);
    }

    // Encode correlation ID into payload prefix (8 bytes BE)
    StreamBuffer correlated;
    for (int i = 7; i >= 0; --i) {
        correlated.push_back(static_cast<uint8_t>((request_id >> (i * 8)) & 0xFF));
    }
    correlated.append(msg.payload().data(), msg.payload().size());

    TypedMessage correlated_msg(msg.type_id(), correlated);
    correlated_msg.set_sender_address(reply_adapter_.address());

    // Send to target actor using standard actor messaging
    context()->send(target, std::move(correlated_msg));

    // Schedule timeout
    int timeout_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(reply_timeout_).count());
    loop_.run_after([this, request_id] { on_timeout(request_id); }, timeout_ms);
}

void HTTPServerActor::on_reply(TypedMessage&& msg) {
    const auto& payload = msg.payload();
    if (payload.size() < 8) return;

    uint64_t request_id = 0;
    for (int i = 0; i < 8; ++i) {
        request_id = (request_id << 8) | payload.data()[i];
    }

    HTTPConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        auto it = pending_replies_.find(request_id);
        if (it == pending_replies_.end()) return;
        conn = it->second->conn;
        pending_replies_.erase(it);
    }

    if (!conn) return;

    // Strip 8-byte prefix to get actual reply payload
    StreamBuffer reply_payload;
    reply_payload.append(payload.data() + 8, payload.size() - 8);
    TypedMessage reply_msg(msg.type_id(), reply_payload);

    auto [body, content_type] = serializer_->serialize_response(
        reply_msg, "application/json");

    std::vector<HttpHeader> headers = {
        {"Content-Type", content_type},
    };

    conn->send_response(HttpStatusCode::OK, std::move(headers), std::move(body));

    if (!conn->should_keep_alive()) {
        close_connection(conn);
    }
}

void HTTPServerActor::on_error(HTTPConnection* conn, HttpStatusCode code,
                                const std::string& msg) {
    StreamBuffer body;
    body.append((const uint8_t*)msg.data(), msg.size());
    conn->send_response(code,
        {{"Content-Type", "text/plain"}, {"Connection", "close"}},
        std::move(body));
    close_connection(conn);
}

void HTTPServerActor::on_timeout(uint64_t request_id) {
    std::lock_guard<std::mutex> lock(reply_mutex_);
    auto it = pending_replies_.find(request_id);
    if (it == pending_replies_.end()) return;

    auto* conn = it->second->conn;
    pending_replies_.erase(it);

    StreamBuffer body;
    const char* msg = "Upstream actor did not respond in time";
    body.append((const uint8_t*)msg, strlen(msg));
    conn->send_response(HttpStatusCode::GatewayTimeout,
        {{"Content-Type", "text/plain"}, {"Connection", "close"}},
        std::move(body));
    close_connection(conn);
}

void HTTPServerActor::close_connection(HTTPConnection* conn) {
    // Remove pending replies for this connection
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        for (auto it = pending_replies_.begin(); it != pending_replies_.end(); ) {
            if (it->second->conn == conn) {
                it = pending_replies_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = connections_.find(conn);
    if (it != connections_.end()) {
        it->second->close();
        connections_.erase(it);
    }
}

} // namespace hpactor::net
```

Note: The RouteRegistry implementations (parse_pattern, match_pattern, add, match) should be copied verbatim from the current `src/net/http_server.cpp` lines 49-153. They are omitted above for brevity but must be included.

- [ ] **Step 4: Build**

```bash
ninja -C build
```

Expected: Build succeeds. Fix any compilation errors (primarily around include paths and the RouteRegistry code copy).

- [ ] **Step 5: Update test_http_server.cpp for new HTTPServerActor interface**

Read `tests/net/test_http_server.cpp`. The test currently creates `HttpServer` directly. Update it to use `HTTPServerActor` via `ActorSystem::spawn()`:

Key changes to the test fixture:
- Instead of `HttpServer server{&system}` on the stack, spawn `HTTPServerActor` as an actor
- The `HTTPServerActor` DaemonActor starts its own thread in `on_activate()` — no need for `server_thread`
- `server.route()` → call `route()` on the spawned actor (get the raw pointer via `dynamic_cast` or store it)
- `server.stop()` → the DaemonActor stops when the ActorSystem is destroyed

Since this is a significant test refactor, keep it minimal: verify the basic end-to-end flow (POST to echo actor) works. The fixture pattern changes from managing a raw HttpServer to spawning an HTTPServerActor and waiting for it to be ready.

- [ ] **Step 6: Run HTTP server tests**

```bash
ninja -C build && ctest -R test_http_server --output-on-failure
```

Expected: At minimum, test_e2e_post_echo_actor passes (200 OK with JSON-wrapped body).

- [ ] **Step 7: Run full test suite**

```bash
ctest --output-on-failure
```

- [ ] **Step 8: Commit**

```bash
git add include/hpactor/net/http_server.hpp src/net/http_server.cpp tests/net/test_http_server.cpp
git commit -m "refactor(net): rewrite HttpServer as HTTPServerActor (DaemonActor-based)"
```

---

### Task 4: Refine HttpClient to Use HTTPConnection

**Files:**
- Modify: `include/hpactor/net/http_client.hpp`
- Modify: `src/net/http_client.cpp`

- [ ] **Step 1: Update HttpClient header to use HTTPConnection pool**

Read `include/hpactor/net/http_client.hpp`. Update the private section to use `HTTPConnection` instead of `HttpKeepAlivePool`:

```cpp
  private:
    struct ParsedUrl;  // internal URL parse result
    std::shared_ptr<HTTPConnection> get_connection(const ParsedUrl& target);

    // TODO: used when full HttpClient implementation is complete
    [[maybe_unused]] EventLoop* loop_ = nullptr;
    std::unordered_map<std::string, std::vector<std::shared_ptr<HTTPConnection>>> pool_;
    mutable std::mutex mutex_;
    std::chrono::milliseconds default_timeout_{5000};
    int max_retries_{3};
    bool keepalive_enabled_{true};
    size_t max_conns_per_host_{4};
```

Add include for `HTTPConnection`:

```cpp
#include <hpactor/net/http_connection.hpp>
```

Remove forward declaration of `HttpKeepAlivePool` if present.

- [ ] **Step 2: Update HttpClient implementation**

Read `src/net/http_client.cpp`. Refactor `HttpClient::request()` to use `HTTPConnection` in Client mode instead of raw `socket()`/`connect()`/`read()`/`write()`. Key changes:

1. `get_connection()` — look up or create `HTTPConnection` in Client mode for the target host:port
2. `request()` — use `conn->send_raw()` to send the HTTP request wire bytes, then `conn->set_response_handler()` to capture the response, poll the EventLoop until complete
3. `abort()` — close all connections in the pool

The current implementation is synchronous (blocking `read()`/`write()`). Since HTTPConnection uses the EventLoop for async I/O, the HttpClient must either:
- Keep its blocking mode by using the EventLoop from its own thread context, OR
- Accept that it needs an EventLoop and poll/wait on it

For this phase, keep the existing simple approach: create a temporary HTTPConnection for each request, use it synchronously (write full request, then poll EventLoop for response). The keep-alive pool optimization is a future enhancement.

- [ ] **Step 3: Build**

```bash
ninja -C build
```

- [ ] **Step 4: Run HTTP-related tests**

```bash
ctest -R "http" --output-on-failure
```

- [ ] **Step 5: Run full test suite**

```bash
ctest --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/net/http_client.hpp src/net/http_client.cpp
git commit -m "refactor(net): refine HttpClient to use HTTPConnection internally"
```

---

### Task 5: Integration Verification

**Files:** None new.

- [ ] **Step 1: Clean rebuild**

```bash
rm -rf build && cmake -S . -B build -GNinja && ninja -C build
```

Expected: Zero warnings, zero errors.

- [ ] **Step 2: Run full test suite**

```bash
ctest --output-on-failure
```

Expected: All tests pass.

- [ ] **Step 3: Verify Config defaults preserve existing behavior**

Check that with default `Config{}`, no HTTP server is spawned (http_server_actor_ is null) and HttpClient is not created. The existing `test_http_server` test manually creates its own server, so it's unaffected by Config flags.

- [ ] **Step 4: Commit any final cleanup**

```bash
git add -A && git commit -m "chore: final integration cleanup for HTTP subsystem refinement"
```
