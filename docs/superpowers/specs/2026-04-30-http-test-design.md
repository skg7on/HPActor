# HTTP Protocol Test Design Specification

**Date:** 2026-04-30
**Status:** Draft (revised after code review)
**Author:** HPActor Team

---

## Overview

Design a comprehensive test suite for the HTTP protocol subsystem. Tests fall into three categories: HTTP/1.1 parser unit tests (HttpParser + llhttp), content negotiation unit tests (HttpSerializer), and end-to-end integration tests (HttpClient → HttpServer → actor → response). Integration tests dogfood the HttpClient by using it to send requests to the HttpServer, validating both ingress and egress simultaneously.

## Goals

- **HttpParser**: Validate HTTP/1.1 parsing — method, URL, headers, body, chunked transfer, keep-alive, upgrade detection, error handling, incremental feed, parser reuse
- **HttpSerializer**: Validate content negotiation — Content-Type deserialization, Accept-based serialization, quality-weight ordering, default formats
- **Integration**: End-to-end HTTP request → actor → HTTP response using real TCP sockets, real EventLoop, and real actor message delivery
- **HttpClient dogfooding**: Build out the HttpClient stub enough to perform real HTTP requests against localhost, validating the client's TCP connect, request wire serialization, response parsing, and `RpcFuture` fulfillment against the HttpServer
- **Follow existing patterns**: No test framework — `int main()` with `cassert`, one test file per concern, registered in `tests/CMakeLists.txt`

## Non-Goals

- HTTPS/TLS testing — plain HTTP only for this phase
- External HTTP server testing — HttpClient is tested against our own HttpServer on localhost
- WebSocket testing
- HTTP/2 testing
- Load/stress testing
- Testing against real external services

---

## Architecture

### Test Executable Organization

```
tests/net/
├── test_http_parser.cpp         # 8 tests — HttpParser unit
├── test_http_serializer.cpp     # 5 tests — HttpSerializer unit
└── test_http_server.cpp         # 6 tests — end-to-end integration
```

### Integration Test Topology

```
┌─ Background Thread ──────────────────────────────────────┐
│                                                           │
│  HttpServer server(system);                               │
│  server.route(POST, "/echo", echo_builder);               │
│  server.route(POST, "/echo/:name", param_builder);        │
│  server.listen(port);  // blocks on EventLoop internally   │
│      │                                                    │
│      └── EventLoop::wait()/process_completions() loop     │
│                                                           │
│  TypedMessage ◄── actor ──► context()->reply()            │
│       ▲                          │                        │
│       │    Route match +         │                        │
│       │    deliver_local()       │                        │
│       │                          ▼                        │
│  ┌──────────┐              ┌──────────┐                  │
│  │ HttpParser│◄── bytes ───│ EventLoop│                  │
│  │ (llhttp)  │              │ (kqueue) │                  │
│  └──────────┘              └────┬─────┘                  │
│                                 │                         │
└─────────────────────────────────┼─────────────────────────┘
                                  │ TCP (127.0.0.1:0)
┌─ Main Thread ───────────────────┼─────────────────────────┐
│                                 │                         │
│  // Wait for server ready       │                         │
│  while (!server.is_running()) std::this_thread::sleep_for(10ms);  │
│                                 │                         │
│  // Send request via HttpClient │                         │
│  HttpClient client(nullptr);    │                         │
│  auto future = client.post(     │                         │
│      "http://127.0.0.1:PORT/echo", body);                 │
│  auto result = future.get();    │                         │
│  assert(result.has_value());    │                         │
│                                 │                         │
│  server.stop();  // exits EventLoop, unblocks bg thread    │
│  bg_thread.join();              │                         │
│                                 │                         │
└────────────────────────────────────────────────────────────┘
```

**Key design details:**

- The background thread calls `server.listen(port)` which blocks internally by calling `EventLoop::wait()` and `process_completions()` in a loop until `stop()` is called. This is a modification to `HttpServer::listen()` — currently it starts the EventLoop but doesn't block. The method must be changed to run the event processing loop.
- The main thread polls `server.is_running()` (a `std::atomic<bool>` member) to know when the server is ready to accept. The current `http_server.hpp` declares `bool running_` — this must be changed to `std::atomic<bool> running_` for correct cross-thread visibility.
- The HttpClient runs on the main thread with blocking socket I/O — no EventLoop needed for the client side. The `nullptr` loop argument is valid because the blocking code path does not dereference `loop_`.
- The `HttpServer::stop()` method sets `running_ = false` and calls `loop_.stop()`, which causes the `wait()` to return, exiting the processing loop in the background thread.
- Test fixture uses port pre-discovery (bind a throwaway socket to port 0, read the assigned port, close) as a test-only workaround. Production use of `listen(0)` for dynamic ports would require `getsockname()` after bind within `listen()` itself.

### HttpServer::listen() Modification

The `listen()` method must be changed from non-blocking to blocking. After binding the socket and registering the accept handler, it enters an event processing loop:

```cpp
void HttpServer::listen(uint16_t port, std::string host) {
    // 1. Create and bind socket (existing code)
    // ... socket(), setsockopt(), fcntl(), bind(), ::listen() ...

    bound_port_ = port;

    // 2. Register accept handler (must happen BEFORE the event loop)
    loop_.add_fd(listening_fd_, EventLoop::Event::Read);
    loop_.set_read_handler(listening_fd_, [this](int /*fd*/) {
        // accept loop (existing code)
    });

    // 3. Set running flag AFTER handlers are registered and socket is bound.
    // is_running() is std::atomic<bool> — visible to other threads.
    running_.store(true, std::memory_order_release);

    // 4. Block and process events until stop() is called
    while (running_.load(std::memory_order_acquire)) {
        loop_.wait(100);                // block up to 100ms for events
        loop_.process_completions();    // dispatch ready completions
    }
}
```

This replaces the `loop_.run()` call with an explicit wait/process loop that stays alive until `stop()` sets `running_ = false`. The handler registration must be placed BEFORE the event processing loop — the current code registers handlers after `loop_.run()`, which must be reordered accordingly. This pattern matches how `test_event_loop.cpp` drives the EventLoop in tests.

### HttpParser Response-Mode Support

The `HttpParser` currently hardcodes `HTTP_REQUEST`. For HttpClient response parsing, add a mode parameter:

```cpp
// In http_parser.hpp:
enum class HttpParserMode { Request, Response };

// Add mode to constructor:
HttpParser(HttpParserMode mode = HttpParserMode::Request);
```

When in Response mode, the `on_message_complete_cb` captures the HTTP status code (via `llhttp_get_status_code(&parser_)`) and stores it. A separate `ResponseCallback` type captures status + headers + body, rather than reusing the `HttpRequest` struct:

```cpp
// In http_parser.hpp:
using ResponseCallback = std::function<void(int status_code,
    const std::vector<HttpHeader>& headers, const bytes& body)>;

void set_on_response(ResponseCallback cb) { on_response_ = std::move(cb); }
```

The `on_message_complete_cb` dispatches to either `on_message_` (request mode) or `on_response_` (response mode) based on the mode setting.

### HttpClient Build-Out

The current stub must become minimally functional for localhost integration testing. This is **not** a full production implementation — it's the minimal path for test usage.

**Minimal additions to `HttpClient::request()`:**

1. `parse_url(url)` → host, port, path (already in stub, removed `[[maybe_unused]]`; uses `std::stoi` for port — assumes well-formed numeric ports, does not validate. Test URLs use only numeric ports.)
2. `connect(host, port)` → fd (raw socket connect, blocking)
3. Serialize request → HTTP/1.1 wire bytes (method + path + headers + body)
4. `write(fd, wire_bytes)` (blocking send)
5. Read response via `HttpParser(HttpParserMode::Response)`
6. Fulfill `std::promise<result<bytes>>` with response body
7. `close(fd)`

The HttpClient in test context uses blocking I/O — no EventLoop needed. Each `request()` opens a fresh connection for the request. This means the keepalive integration test (two requests on the same connection) must use a raw-socket approach rather than the HttpClient convenience methods.

### New Error Codes

Add HTTP-specific error codes to the `errors` namespace in `types.hpp`:

```cpp
namespace errors {
constexpr uint32_t unknown = 1;
constexpr uint32_t actor_down = 2;
constexpr uint32_t actor_not_found = 3;
constexpr uint32_t mailbox_full = 4;
constexpr uint32_t timeout = 5;
constexpr uint32_t http_connect_failed = 2002;   // NEW
constexpr uint32_t http_timeout = 2003;           // NEW
constexpr uint32_t http_parse_error = 2001;       // NEW
constexpr uint32_t user = 1000;
}
```

---

## Test Cases

### test_http_parser.cpp (8 tests)

| # | Test | Input | Expected |
|---|------|-------|----------|
| 1 | `parse_simple_get` | `"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"` | method=GET, path="/", headers=[Host:localhost], body empty, keep_alive=true |
| 2 | `parse_post_with_body` | `"POST /api/data HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello"` | method=POST, path="/api/data", body="hello" |
| 3 | `parse_chunked` | `"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"` | body="hello", message_complete fires |
| 4 | `parse_keepalive_detection` | Two calls: A with `Connection: keep-alive`, B with `Connection: close` | A: keep_alive=true, B: keep_alive=false |
| 5 | `parse_error_malformed` | `"NOTHTTP\r\n\r\n"` | parser state=Error, on_error callback invoked |
| 6 | `parser_reset_and_reuse` | Parse request A, call reset(), parse request B | Both parse independently, no state leakage |
| 7 | `parse_incremental_feed` | Feed `"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"` 3-5 bytes at a time | Complete parse after final chunk, data accumulated correctly across partial feeds |
| 8 | `parse_upgrade_detection` | Request with `Upgrade: websocket` and `Connection: upgrade` | `upgrade_requested()` returns true |

**Incremental feed test pattern (Test 7):**

```cpp
void test_parse_incremental_feed() {
    HttpParser parser;
    bool received = false;
    parser.set_on_message([&](HttpRequest&&) { received = true; });

    const char* raw = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t len = strlen(raw);

    // Feed 3 bytes at a time
    for (size_t i = 0; i < len; i += 3) {
        size_t chunk = std::min(size_t(3), len - i);
        std::span<const uint8_t> data(
            reinterpret_cast<const uint8_t*>(raw + i), chunk);
        parser.execute(data);
    }

    assert(received);
    assert(parser.state() == HttpParseState::Complete);
}
```

### test_http_serializer.cpp (5 tests)

| # | Test | Input | Expected |
|---|------|-------|----------|
| 9 | `deserialize_json_content_type` | HttpRequest with Content-Type: application/json, body `{"x":1}` | Returns TypedMessage with expected_tag, body preserved |
| 10 | `deserialize_protobuf_content_type` | HttpRequest with Content-Type: application/x-protobuf, body raw proto bytes | Returns TypedMessage with expected_tag, body = raw bytes |
| 11 | `serialize_response_accept_json` | TypedMessage, Accept: "application/json" | Returns (json_bytes, "application/json; charset=utf-8") |
| 12 | `serialize_accept_quality_weights` | TypedMessage, Accept: "text/plain;q=0.5, application/json;q=0.8" | JSON chosen (higher quality) |
| 13 | `serialize_no_accept_default_json` | TypedMessage, Accept: "" | Returns (json_bytes, "application/json; charset=utf-8") |

### test_http_server.cpp (6 tests — integration)

| # | Test | Flow | Expected |
|---|------|------|----------|
| 14 | `e2e_post_echo_actor` | HttpClient::post("/echo", "hello") → echo actor → reply | 200 OK, body = "hello" |
| 15 | `e2e_missing_route_404` | HttpClient::post("/nonexistent", body) → no route | HTTP 404 Not Found |
| 16 | `e2e_actor_timeout_504` | HttpClient::post("/slow", body) → no actor receives (deliberately invalid ActorId), timeout fires after default 5s | HTTP 504 Gateway Timeout |
| 17 | `e2e_invalid_http_400` | Raw socket: write "GARBAGE\r\n\r\n", read response | HTTP 400 Bad Request |
| 18 | `e2e_keepalive_two_requests` | Raw socket: POST /echo twice on same fd, read both responses | Both return 200 OK, connection remains open (no RST) |
| 19 | `e2e_named_route_params` | POST /echo/test-user with route "/echo/:name" | Actor receives path_params["name"]="test-user", 200 OK with echoed name in body |

**Integration test fixture:**

```cpp
struct HttpTestFixture {
    ActorSystem system{Config{}};     // enable_network=false (default)
    HttpServer server{&system};
    Actor echo_actor;
    std::thread server_thread;
    uint16_t port = 0;

    HttpTestFixture() {
        echo_actor = system.spawn<EchoActor>();
        setup_routes();
        start_server();
    }

    void setup_routes() {
        // POST /echo — simple echo
        server.route(HttpMethod::POST, "/echo",
            [this](const HttpRequest& req) -> std::pair<ActorAddress, TypedMessage> {
                TypedMessage msg(TypeTag::User, req.body);
                return {echo_actor.address(), std::move(msg)};
            });

        // POST /echo/:name — named param echo
        server.route(HttpMethod::POST, "/echo/:name",
            [this](const HttpRequest& req) -> std::pair<ActorAddress, TypedMessage> {
                auto it = req.path_params.find("name");
                std::string name = (it != req.path_params.end()) ? it->second : "unknown";
                bytes body;
                body.append(reinterpret_cast<const uint8_t*>(name.data()), name.size());
                TypedMessage msg(TypeTag::User, body);
                return {echo_actor.address(), std::move(msg)};
            });

        // POST /slow — timeout test (no route means 404; register a route
        // that routes to a non-existent actor to trigger timeout)
        // Alternative: set reply_timeout to a very short duration and have
        // the actor not reply.
        server.route(HttpMethod::POST, "/slow",
            [this](const HttpRequest& req) -> std::pair<ActorAddress, TypedMessage> {
                // Route to a separate actor that never replies
                ActorAddress no_reply_addr(
                    LocalEndpoint, ActorType{99}, ActorId{99999}, 0);
                TypedMessage msg(TypeTag::User, req.body);
                return {no_reply_addr, std::move(msg)};
            });
    }

    void start_server() {
        // Find available port
        int test_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;
        socklen_t len = sizeof(addr);
        bind(test_fd, (struct sockaddr*)&addr, len);
        getsockname(test_fd, (struct sockaddr*)&addr, &len);
        port = ntohs(addr.sin_port);
        close(test_fd);

        server_thread = std::thread([this] {
            server.listen(port);  // blocks until stop()
        });

        // Wait for server to be truly ready (is_running() atomically set
        // after socket is bound and EventLoop is processing)
        while (!server.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~HttpTestFixture() {
        server.stop();
        if (server_thread.joinable()) server_thread.join();
    }
};
```

**Echo actor:**

```cpp
class EchoActor : public EventBasedActor {
    EchoActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            TypedMessage reply(msg.type_id(), msg.payload());
            context()->reply(std::move(reply));
        });
    }
};
```

**Integration test patterns:**

```cpp
// Standard: HttpClient-driven test
void test_e2e_post_echo_actor() {
    HttpTestFixture f;

    HttpClient client(nullptr);
    std::string url = "http://127.0.0.1:" + std::to_string(f.port) + "/echo";
    bytes body;
    const char* payload = "hello";
    body.append(reinterpret_cast<const uint8_t*>(payload), 5);

    auto future = client.post(url, std::move(body));
    auto result = future.get();  // blocks with 5s timeout (set at construction)

    assert(result.has_value());
    const auto& resp_bytes = result.value();
    std::string resp_str(resp_bytes.begin(), resp_bytes.end());
    assert(resp_str == "hello");
}

// Keepalive: raw-socket test (HttpClient doesn't support keepalive yet)
void test_e2e_keepalive_two_requests() {
    HttpTestFixture f;

    // Connect once
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(f.port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, (struct sockaddr*)&addr, sizeof(addr));

    // Send first request
    const char* req1 = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello";
    write(fd, req1, strlen(req1));
    // Read first response
    char buf1[4096];
    ssize_t n1 = read(fd, buf1, sizeof(buf1) - 1);
    buf1[n1] = '\0';
    assert(strstr(buf1, "200 OK") != nullptr);

    // Send second request on same fd
    const char* req2 = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nworld";
    write(fd, req2, strlen(req2));
    char buf2[4096];
    ssize_t n2 = read(fd, buf2, sizeof(buf2) - 1);
    buf2[n2] = '\0';
    assert(strstr(buf2, "200 OK") != nullptr);

    // Connection should still be open (no RST)
    close(fd);
}

// Invalid HTTP: raw-socket test (HttpClient always sends valid HTTP)
void test_e2e_invalid_http_400() {
    HttpTestFixture f;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(f.port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, (struct sockaddr*)&addr, sizeof(addr));

    const char* garbage = "GARBAGE\r\n\r\n";
    write(fd, garbage, strlen(garbage));

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    assert(strstr(buf, "400") != nullptr);

    close(fd);
}
```

---

## Error Handling

- Fixture destructor calls `server.stop()` + `loop_.stop()` which exits the EventLoop processing loop, then joins the background thread
- Sockets are closed in all code paths (including error paths in HttpClient::request())
- `RpcFuture::get()` blocks with the timeout set at construction (5s default) — prevents indefinite hangs
- Background thread properly joins in fixture destructor even if assertions fail
- Actor spawn uses the EchoActor which always replies — no orphan messages
- The `/slow` route sends to a non-existent ActorId to trigger timeout naturally

---

## Thread Safety

| Component | Thread | Notes |
|-----------|--------|-------|
| HttpServer + EventLoop | Background thread | Single-threaded I/O processing |
| ActorSystem + scheduler | Background thread (via EventLoop callbacks) | `deliver_local()` invoked from HttpServer's EventLoop thread |
| HttpClient | Main test thread | Blocking I/O, no concurrency |
| Atomic flag (`running_`) | Both | `std::atomic<bool>` in HttpServer for safe cross-thread communication |

**No shared mutable state** between main thread and background thread beyond the atomic flag. The EchoActor is immutably spawned and registered before the server starts.

---

## Build Integration

Add to `tests/CMakeLists.txt`:

```cmake
# HTTP parser tests
add_executable(test_http_parser net/test_http_parser.cpp)
target_link_libraries(test_http_parser hpactor)
add_test(NAME test_http_parser COMMAND test_http_parser)

# HTTP serializer tests
add_executable(test_http_serializer net/test_http_serializer.cpp)
target_link_libraries(test_http_serializer hpactor)
add_test(NAME test_http_serializer COMMAND test_http_serializer)

# HTTP server integration tests
add_executable(test_http_server net/test_http_server.cpp)
target_link_libraries(test_http_server hpactor)
add_test(NAME test_http_server COMMAND test_http_server)
```

---

## Files

| File | Purpose | New/Modify |
|------|---------|-----------|
| `tests/net/test_http_parser.cpp` | HttpParser unit tests (8 tests) | **New** |
| `tests/net/test_http_serializer.cpp` | HttpSerializer unit tests (5 tests) | **New** |
| `tests/net/test_http_server.cpp` | Integration tests (6 tests) | **New** |
| `src/net/http_client.cpp` | Build out HttpClient::request() with real TCP | **Modify** |
| `include/hpactor/net/http_parser.hpp` | Add HttpParserMode enum, ResponseCallback, response-mode ctor | **Modify** |
| `src/net/http_parser.cpp` | Support HTTP_RESPONSE mode in llhttp init and callbacks | **Modify** |
| `include/hpactor/net/http_server.hpp` | Change `bool running_` to `std::atomic<bool> running_` for cross-thread visibility | **Modify** |
| `src/net/http_server.cpp` | Modify `listen()` to block on EventLoop wait/process loop; reorder handler registration before the event loop | **Modify** |
| `include/hpactor/types/types.hpp` | Add `http_connect_failed`, `http_timeout`, `http_parse_error` error codes | **Modify** |
| `tests/CMakeLists.txt` | Add 3 test targets | **Modify** |

## Test Summary

| Test File | Tests | Type |
|-----------|-------|------|
| `test_http_parser` | 8 | Unit — HTTP/1.1 parsing |
| `test_http_serializer` | 5 | Unit — content negotiation |
| `test_http_server` | 6 | Integration — end-to-end HTTP ↔ actor |
| **Total** | **19** | |
