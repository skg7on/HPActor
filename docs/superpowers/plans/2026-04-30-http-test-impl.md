# HTTP Protocol Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement 19 tests across 3 test executables for the HTTP protocol subsystem. Unit-test HttpParser (llhttp integration) and HttpSerializer (content negotiation). Integration-test the full HTTP request → actor → response flow using a built-out HttpClient and a background-thread HttpServer. Modify HttpServer::listen() to block on an internal EventLoop processing loop.

**Architecture:** Background thread runs HttpServer with blocking EventLoop wait/process loop. Main thread sends requests via HttpClient (blocking I/O, no EventLoop). HttpParser gains Response-mode support for client-side response parsing. HttpServer gets `std::atomic<bool> running_` for cross-thread coordination.

**Tech Stack:** C++20, no exceptions, no RTTI, existing EventLoop/ActorSystem/HttpServer infrastructure, llhttp for parsing, raw POSIX sockets for keepalive/invalid-HTTP tests.

---

## File Structure

```
tests/net/
├── test_http_parser.cpp         # NEW: 8 unit tests
├── test_http_serializer.cpp     # NEW: 5 unit tests
└── test_http_server.cpp         # NEW: 6 integration tests

include/hpactor/
├── net/
│   ├── http_parser.hpp          # MODIFY: add HttpParserMode, ResponseCallback, response-mode ctor
│   └── http_server.hpp          # MODIFY: bool → std::atomic<bool> running_
└── types/
    └── types.hpp                # MODIFY: add HTTP error codes

src/net/
├── http_parser.cpp              # MODIFY: support HTTP_RESPONSE mode
├── http_server.cpp              # MODIFY: listen() blocks on wait/process loop
└── http_client.cpp              # MODIFY: build out request() with real TCP

tests/CMakeLists.txt             # MODIFY: add 3 test targets
```

---

## Task 1: Add HTTP Error Codes

**Files:**
- Modify: `include/hpactor/types/types.hpp`

- [ ] **Step 1: Add error codes to `errors` namespace**

At `include/hpactor/types/types.hpp:306`, after `errors::timeout`, add:

```cpp
namespace errors {
constexpr uint32_t unknown = 1;
constexpr uint32_t actor_down = 2;
constexpr uint32_t actor_not_found = 3;
constexpr uint32_t mailbox_full = 4;
constexpr uint32_t timeout = 5;

// HTTP protocol errors
constexpr uint32_t http_connect_failed = 2002;
constexpr uint32_t http_parse_error = 2001;
constexpr uint32_t http_timeout = 2003;

constexpr uint32_t user = 1000;
}
```

- [ ] **Step 2: Verify build**

```bash
ninja -C build
```

Expected: PASS

---

## Task 2: Add HttpParser Response-Mode Support

**Files:**
- Modify: `include/hpactor/net/http_parser.hpp`
- Modify: `src/net/http_parser.cpp`

- [ ] **Step 1: Add HttpParserMode enum and ResponseCallback to header**

In `http_parser.hpp`, before the `HttpParser` class, add:

```cpp
enum class HttpParserMode { Request, Response };

// ResponseCallback fired in Response mode — carries status, headers, body.
using ResponseCallback = std::function<void(int status_code,
    const std::vector<HttpHeader>& headers, const bytes& body)>;
```

- [ ] **Step 2: Add mode-aware constructor and setter**

Replace the current constructor declaration with:

```cpp
explicit HttpParser(HttpParserMode mode = HttpParserMode::Request);
```

Add to public section:

```cpp
void set_on_response(ResponseCallback cb) { on_response_ = std::move(cb); }
```

Add to private members:

```cpp
HttpParserMode mode_ = HttpParserMode::Request;
ResponseCallback on_response_;
```

- [ ] **Step 3: Update HttpParser constructor and llhttp init in cpp**

In `http_parser.cpp`, change the constructor to accept mode:

```cpp
HttpParser::HttpParser(HttpParserMode mode) : mode_(mode) {
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = on_message_begin_cb;
    settings_.on_url = on_url_cb;
    settings_.on_method = on_method_cb;
    settings_.on_header_field = on_header_field_cb;
    settings_.on_header_value = on_header_value_cb;
    settings_.on_headers_complete = on_headers_complete_cb;
    settings_.on_body = on_body_cb;
    settings_.on_message_complete = on_message_complete_cb;

    auto llhttp_type = (mode == HttpParserMode::Request) ? HTTP_REQUEST
                                                          : HTTP_RESPONSE;
    llhttp_init(&parser_, llhttp_type, &settings_);
    parser_.data = this;
}
```

Update `reset()` to preserve the mode:

```cpp
void HttpParser::reset() {
    auto llhttp_type = (mode_ == HttpParserMode::Request) ? HTTP_REQUEST
                                                            : HTTP_RESPONSE;
    llhttp_init(&parser_, llhttp_type, &settings_);
    parser_.data = this;
    // ... rest of reset ...
}
```

- [ ] **Step 4: Update on_message_complete_cb to dispatch by mode**

```cpp
int HttpParser::on_message_complete_cb(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->state_ = HttpParseState::Complete;

    if (self->mode_ == HttpParserMode::Response) {
        if (self->on_response_) {
            int status = llhttp_get_status_code(parser);
            self->on_response_(status, std::move(self->headers_),
                                std::move(self->body_buf_));
        }
    } else {
        if (self->on_message_) {
            HttpRequest req;
            req.method = self->method_;
            req.path = std::move(self->url_buf_);
            req.headers = std::move(self->headers_);
            req.http_major = self->http_major_;
            req.http_minor = self->http_minor_;
            req.body = std::move(self->body_buf_);
            self->on_message_(std::move(req));
        }
    }
    return 0;
}
```

- [ ] **Step 5: Verify build**

```bash
ninja -C build
```

Expected: PASS (no new code uses response mode yet, just verify no regressions)

---

## Task 3: Modify HttpServer for Blocking listen() and Atomic running_

**Files:**
- Modify: `include/hpactor/net/http_server.hpp`
- Modify: `src/net/http_server.cpp`

- [ ] **Step 1: Change `bool running_` to `std::atomic<bool> running_`**

In `http_server.hpp`, change line 115 from:

```cpp
bool running_ = false;
```

to:

```cpp
std::atomic<bool> running_{false};
```

Add `#include <atomic>` to includes if not already present.

- [ ] **Step 2: Rewrite `listen()` to block with EventLoop processing loop**

Replace the event loop setup in `listen()` (currently: `running_ = true; loop_.run();` followed by handler registration). The new ordering must be: bind → register handlers → set running → event loop.

```cpp
void HttpServer::listen(uint16_t port, std::string host) {
    // 1. Create and bind socket (keep existing code)
    listening_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listening_fd_ < 0) return;
    int opt = 1;
    setsockopt(listening_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    fcntl(listening_fd_, F_SETFL, O_NONBLOCK);
    // ... bind, ::listen() (existing code) ...

    bound_port_ = port;

    // 2. Register accept handler BEFORE entering the event loop
    loop_.add_fd(listening_fd_, EventLoop::Event::Read);
    loop_.set_read_handler(listening_fd_, [this](int /*fd*/) {
        struct sockaddr_in client_addr {};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(
            listening_fd_, reinterpret_cast<struct sockaddr*>(&client_addr),
            &client_len);
        if (client_fd < 0) return;
        fcntl(client_fd, F_SETFL, O_NONBLOCK);

        if (connections_.size() >= max_connections_) {
            close(client_fd);
            return;
        }

        auto ctx = std::make_unique<ConnectionCtx>();
        ctx->fd = client_fd;
        ctx->parser = std::make_unique<HttpParser>();
        ctx->parser->set_on_message(
            [this, fd = client_fd](HttpRequest&& req) {
                on_parse_complete(fd, std::move(req));
            });
        ctx->parser->set_on_error(
            [this, fd = client_fd](llhttp_errno_t /*err*/, const char* msg) {
                send_error(fd, HttpStatusCode::BadRequest,
                           msg ? msg : "Parse error");
            });
        connections_[client_fd] = std::move(ctx);

        loop_.add_fd(client_fd, EventLoop::Event::Read);
        loop_.set_read_handler(client_fd,
                               [this](int cfd) { on_read(cfd); });
    });

    // 3. Signal ready (atomic, visible to other threads polling is_running())
    running_.store(true, std::memory_order_release);

    // 4. Block processing events until stop() is called
    while (running_.load(std::memory_order_acquire)) {
        loop_.wait(100);
        loop_.process_completions();
    }
}
```

Remove the old `loop_.run()` call (line 251 in current code).

- [ ] **Step 3: Update `stop()` to use atomic store**

```cpp
void HttpServer::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    loop_.stop();
    // ... close connections (existing code) ...
}
```

- [ ] **Step 4: Update `is_running()`**

The accessor `bool is_running() const { return running_; }` already works since `std::atomic<bool>` implicitly converts to `bool`. No change needed.

- [ ] **Step 5: Verify build and run existing tests**

```bash
ninja -C build && ctest --output-on-failure
```

Expected: All 62 existing tests pass (HttpServer is not exercised by any existing test, so no regressions expected).

---

## Task 4: Build Out HttpClient::request() with Real TCP

**Files:**
- Modify: `src/net/http_client.cpp`

- [ ] **Step 1: Replace the stub `request()` implementation**

Replace the existing stub (which returns a dummy future) with a real blocking TCP implementation:

```cpp
RpcFuture<bytes>
HttpClient::request(HttpMethod method, const std::string& url,
                    std::vector<HttpHeader> headers, bytes body) {
    auto parsed = parse_url(url);

    // 1. Connect
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::promise<result<bytes>> p;
        p.set_value(result<bytes>::make(error(errors::http_connect_failed,
                                              "socket() failed")));
        return RpcFuture<bytes>(p.get_future(), default_timeout_);
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(parsed.port);
    inet_pton(AF_INET, parsed.host.c_str(), &addr.sin_addr);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        std::promise<result<bytes>> p;
        p.set_value(result<bytes>::make(error(errors::http_connect_failed,
                                              "connect() failed")));
        return RpcFuture<bytes>(p.get_future(), default_timeout_);
    }

    // 2. Build HTTP/1.1 request wire bytes
    bytes wire;
    std::string request_line = to_string(method);
    request_line += " " + parsed.path + " HTTP/1.1\r\n";
    wire.append(reinterpret_cast<const uint8_t*>(request_line.data()),
                request_line.size());

    std::string host_header = "Host: " + parsed.host;
    if (parsed.port != 80) host_header += ":" + std::to_string(parsed.port);
    host_header += "\r\n";
    wire.append(reinterpret_cast<const uint8_t*>(host_header.data()),
                host_header.size());

    for (const auto& h : headers) {
        std::string hdr = h.name + ": " + h.value + "\r\n";
        wire.append(reinterpret_cast<const uint8_t*>(hdr.data()), hdr.size());
    }

    if (!body.empty()) {
        std::string cl = "Content-Length: " + std::to_string(body.size()) + "\r\n";
        wire.append(reinterpret_cast<const uint8_t*>(cl.data()), cl.size());
    }
    wire.append(reinterpret_cast<const uint8_t*>("\r\n"), 2);
    wire.append(body.data(), body.size());

    // 3. Send
    ssize_t sent = write(fd, wire.data(), wire.size());
    if (sent < 0) {
        close(fd);
        std::promise<result<bytes>> p;
        p.set_value(result<bytes>::make(error(errors::http_connect_failed,
                                              "write() failed")));
        return RpcFuture<bytes>(p.get_future(), default_timeout_);
    }

    // 4. Read response via llhttp in HTTP_RESPONSE mode
    HttpParser parser(HttpParserMode::Response);
    int response_status = 0;
    bytes response_body;
    bool complete = false;

    parser.set_on_response(
        [&](int status, const std::vector<HttpHeader>& /*resp_headers*/,
            const bytes& body) {
            response_status = status;
            response_body = body;
            complete = true;
        });

    uint8_t buf[4096];
    ssize_t n;
    while (!complete && (n = read(fd, buf, sizeof(buf))) > 0) {
        parser.execute(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
    }
    close(fd);

    // 5. Fulfill promise
    std::promise<result<bytes>> p;
    if (response_status >= 200 && response_status < 300) {
        p.set_value(result<bytes>::make(std::move(response_body)));
    } else if (response_status == 0) {
        p.set_value(result<bytes>::make(error(errors::http_parse_error,
                                              "No response received")));
    } else {
        p.set_value(result<bytes>::make(error(errors::http_parse_error,
            "HTTP " + std::to_string(response_status))));
    }
    return RpcFuture<bytes>(p.get_future(), default_timeout_);
}
```

- [ ] **Step 2: Remove unused `serializer_` references from the constructor**

The current constructor `HttpClient::HttpClient(EventLoop* loop) : loop_(loop) {}` is fine — no serializer needed for the build-out.

- [ ] **Step 3: Add convenience method implementations**

The `get`, `post`, `put`, `del` stubs already delegate to `request()` — update them to pass `headers` correctly (they already do) and remove any `[[maybe_unused]]` casts from the parameter lists.

- [ ] **Step 4: Verify build**

```bash
ninja -C build
```

Expected: PASS (HttpClient::request() compiles and links against llhttp/errors)

---

## Task 5: Write HttpParser Unit Tests

**Files:**
- Create: `tests/net/test_http_parser.cpp`

- [ ] **Step 1: Write 8 unit tests**

```cpp
#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace hpactor;
using namespace hpactor::net;

// Helper: create a span from a string literal
static std::span<const uint8_t> bytes_from(const char* str) {
    return {reinterpret_cast<const uint8_t*>(str), strlen(str)};
}

void test_parse_simple_get() {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    parser.execute(bytes_from("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    assert(received);
    assert(captured.method == HttpMethod::GET);
    assert(captured.path == "/");
    assert(captured.http_major == 1);
    assert(captured.http_minor == 1);
    assert(captured.headers.size() >= 1);
    assert(captured.body.empty());
    assert(parser.should_keep_alive());
}

void test_parse_post_with_body() {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    parser.execute(bytes_from(
        "POST /api/data HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello"));

    assert(received);
    assert(captured.method == HttpMethod::POST);
    assert(captured.path == "/api/data");
    assert(captured.body.size() == 5);
    std::string body_str(captured.body.begin(), captured.body.end());
    assert(body_str == "hello");
}

void test_parse_chunked() {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    parser.execute(bytes_from(
        "POST / HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "0\r\n\r\n"));

    assert(received);
    assert(captured.body.size() == 5);
    std::string body_str(captured.body.begin(), captured.body.end());
    assert(body_str == "hello");
}

void test_parse_keepalive_detection() {
    // First: Connection: keep-alive
    {
        HttpParser parser;
        parser.execute(bytes_from(
            "GET / HTTP/1.1\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"));
        assert(parser.should_keep_alive());
    }
    // Second: Connection: close
    {
        HttpParser parser;
        parser.execute(bytes_from(
            "GET / HTTP/1.1\r\n"
            "Connection: close\r\n"
            "\r\n"));
        assert(!parser.should_keep_alive());
    }
}

void test_parse_error_malformed() {
    HttpParser parser;
    bool error_received = false;

    parser.set_on_error([&](llhttp_errno_t /*err*/, const char* /*msg*/) {
        error_received = true;
    });

    parser.execute(bytes_from("NOTHTTP\r\n\r\n"));

    assert(error_received);
    assert(parser.state() == HttpParseState::Error);
}

void test_parser_reset_and_reuse() {
    HttpParser parser;
    int count = 0;

    parser.set_on_message([&](HttpRequest&& /*req*/) { ++count; });

    // Parse first request
    parser.execute(bytes_from("GET /a HTTP/1.1\r\n\r\n"));
    assert(count == 1);

    // Reset and parse second request (keep-alive simulation)
    parser.reset();
    parser.execute(bytes_from("GET /b HTTP/1.1\r\n\r\n"));
    assert(count == 2);
    assert(parser.state() == HttpParseState::Complete);
}

void test_parse_incremental_feed() {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    const char* raw = "GET /test HTTP/1.1\r\nHost: example.com\r\n\r\n";
    size_t len = strlen(raw);

    // Feed 4 bytes at a time to simulate TCP chunk delivery
    for (size_t i = 0; i < len; i += 4) {
        size_t chunk = (i + 4 <= len) ? 4 : len - i;
        std::span<const uint8_t> data(
            reinterpret_cast<const uint8_t*>(raw + i), chunk);
        parser.execute(data);
    }

    assert(received);
    assert(captured.method == HttpMethod::GET);
    assert(captured.path == "/test");
}

void test_parse_upgrade_detection() {
    HttpParser parser;

    parser.execute(bytes_from(
        "GET /ws HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: upgrade\r\n"
        "\r\n"));

    assert(parser.upgrade_requested());
}

int main() {
    test_parse_simple_get();
    test_parse_post_with_body();
    test_parse_chunked();
    test_parse_keepalive_detection();
    test_parse_error_malformed();
    test_parser_reset_and_reuse();
    test_parse_incremental_feed();
    test_parse_upgrade_detection();
    return 0;
}
```

- [ ] **Step 2: Verify build and run**

```bash
ninja -C build && ctest --output-on-failure -R test_http_parser
```

Expected: test_http_parser built and passes all 8 assertions.

---

## Task 6: Write HttpSerializer Unit Tests

**Files:**
- Create: `tests/net/test_http_serializer.cpp`

- [ ] **Step 1: Write 5 unit tests**

```cpp
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/net/http_serializer.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <cstring>
#include <string>

using namespace hpactor;
using namespace hpactor::net;

static bytes make_body(const char* str) {
    bytes body;
    body.append(reinterpret_cast<const uint8_t*>(str), strlen(str));
    return body;
}

void test_deserialize_json_content_type() {
    HttpSerializer serializer;

    HttpRequest req;
    req.method = HttpMethod::POST;
    req.path = "/test";
    req.headers = {{"content-type", "application/json"}};
    req.body = make_body("{\"delta\": 5}");

    auto result = serializer.deserialize_request(req, TypeTag::User);
    assert(result.has_value());
    auto& msg = result.value();
    assert(msg.type_id() == TypeTag::User);
    // Body bytes preserved
    assert(msg.payload().size() > 0);
}

void test_deserialize_protobuf_content_type() {
    HttpSerializer serializer;

    uint8_t raw[] = {0x08, 0x05}; // protobuf: field 1 varint = 5
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.path = "/test";
    req.headers = {{"content-type", "application/x-protobuf"}};
    req.body.append(raw, 2);

    auto result = serializer.deserialize_request(req, TypeTag::User);
    assert(result.has_value());
    auto& msg = result.value();
    assert(msg.payload().size() == 2);
    assert(msg.payload().data()[0] == 0x08);
    assert(msg.payload().data()[1] == 0x05);
}

void test_serialize_response_accept_json() {
    HttpSerializer serializer;

    bytes payload = make_body("test-data");
    TypedMessage msg(TypeTag::User, payload);

    auto [body, content_type] = serializer.serialize_response(
        msg, "application/json");

    assert(content_type.find("application/json") != std::string::npos);
    // JSON wrapper includes the data
    assert(body.size() > 0);
}

void test_serialize_accept_quality_weights() {
    HttpSerializer serializer;

    bytes payload = make_body("data");
    TypedMessage msg(TypeTag::User, payload);

    // JSON has q=0.8, text/plain has q=0.5 → JSON should win
    auto [body, content_type] = serializer.serialize_response(
        msg, "text/plain;q=0.5, application/json;q=0.8");

    assert(content_type.find("application/json") != std::string::npos);
}

void test_serialize_no_accept_default_json() {
    HttpSerializer serializer;

    bytes payload = make_body("data");
    TypedMessage msg(TypeTag::User, payload);

    auto [body, content_type] = serializer.serialize_response(msg, "");

    assert(content_type.find("application/json") != std::string::npos);
}

int main() {
    test_deserialize_json_content_type();
    test_deserialize_protobuf_content_type();
    test_serialize_response_accept_json();
    test_serialize_accept_quality_weights();
    test_serialize_no_accept_default_json();
    return 0;
}
```

- [ ] **Step 2: Verify build and run**

```bash
ninja -C build && ctest --output-on-failure -R test_http_serializer
```

Expected: test_http_serializer built and passes all 5 assertions.

---

## Task 7: Write HttpServer Integration Tests

**Files:**
- Create: `tests/net/test_http_server.cpp`

- [ ] **Step 1: Write EchoActor and HttpTestFixture**

```cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/http_server.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <cassert>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

using namespace hpactor;
using namespace hpactor::net;

// =============================================================================
// EchoActor — echoes received messages back via context()->reply()
// =============================================================================
class EchoActor final : public EventBasedActor {
  public:
    EchoActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            TypedMessage reply(msg.type_id(), msg.payload());
            context()->reply(std::move(reply));
        });
    }
};

// =============================================================================
// SlowActor — never replies (used for timeout tests)
// =============================================================================
class SlowActor final : public EventBasedActor {
  public:
    SlowActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& /*msg*/) {
            // Intentionally never calls reply() — triggers timeout
        });
    }
};

// =============================================================================
// HttpTestFixture — sets up ActorSystem, HttpServer, echo actor on bg thread
// =============================================================================
struct HttpTestFixture {
    ActorSystem system{Config{}};
    HttpServer server{&system};
    Actor echo_actor;
    std::thread server_thread;
    uint16_t port = 0;

    HttpTestFixture() {
        echo_actor = system.spawn<EchoActor>();
        auto slow = system.spawn<SlowActor>();
        (void)slow;

        setup_routes();
        start_server();
    }

    ~HttpTestFixture() {
        server.stop();
        if (server_thread.joinable()) server_thread.join();
    }

  private:
    void setup_routes() {
        // POST /echo — simple echo
        server.route(HttpMethod::POST, "/echo",
            [this](const HttpRequest& req)
                -> std::pair<ActorAddress, TypedMessage> {
                TypedMessage msg(TypeTag::User, req.body);
                return {echo_actor.address(), std::move(msg)};
            });

        // POST /echo/:name — named param echo
        server.route(HttpMethod::POST, "/echo/:name",
            [this](const HttpRequest& req)
                -> std::pair<ActorAddress, TypedMessage> {
                auto it = req.path_params.find("name");
                std::string name = (it != req.path_params.end())
                                       ? it->second : "unknown";
                bytes body;
                body.append(reinterpret_cast<const uint8_t*>(name.data()),
                            name.size());
                TypedMessage msg(TypeTag::User, body);
                return {echo_actor.address(), std::move(msg)};
            });

        // POST /slow — routes to an actor that never replies → triggers 504
        server.route(HttpMethod::POST, "/slow",
            [this](const HttpRequest& req)
                -> std::pair<ActorAddress, TypedMessage> {
                // Route to a non-existent actor — timer fires after reply_timeout
                ActorAddress no_one(
                    LocalEndpoint, ActorType{99}, ActorId{99999}, 0);
                TypedMessage msg(TypeTag::User, req.body);
                return {no_one, std::move(msg)};
            });
    }

    void start_server() {
        // Find available port via OS-assigned port 0
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

        // Wait for server to be truly accepting connections
        while (!server.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

// Helper to create bytes from a string literal
static bytes make_body(const char* str) {
    bytes body;
    body.append(reinterpret_cast<const uint8_t*>(str), strlen(str));
    return body;
}
```

- [ ] **Step 2: Write 6 integration tests**

```cpp
void test_e2e_post_echo_actor() {
    HttpTestFixture f;

    HttpClient client(nullptr);
    std::string url = "http://127.0.0.1:" + std::to_string(f.port) + "/echo";
    auto future = client.post(url, make_body("hello"));

    auto result = future.get();
    assert(result.has_value());
    const auto& resp = result.value();
    std::string resp_str(resp.begin(), resp.end());
    assert(resp_str == "hello");
}

void test_e2e_missing_route_404() {
    HttpTestFixture f;

    HttpClient client(nullptr);
    std::string url = "http://127.0.0.1:" + std::to_string(f.port) + "/nope";
    auto future = client.post(url, make_body("data"));

    auto result = future.get();
    assert(!result.has_value()); // error — 404 mapped to error
}

void test_e2e_actor_timeout_504() {
    HttpTestFixture f;
    // Short reply timeout for faster test
    f.server.set_reply_timeout(std::chrono::milliseconds(200));

    HttpClient client(nullptr);
    std::string url = "http://127.0.0.1:" + std::to_string(f.port) + "/slow";
    auto future = client.post(url, make_body("data"));

    auto result = future.get(); // blocks ~5s (client timeout), but server sends 504 sooner
    assert(!result.has_value()); // error — 504 mapped to error
}

void test_e2e_invalid_http_400() {
    HttpTestFixture f;

    // Raw socket — HttpClient always sends valid HTTP
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(f.port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    assert(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) >= 0);

    const char* garbage = "GARBAGE\r\n\r\n";
    ssize_t sent = write(fd, garbage, strlen(garbage));
    assert(sent > 0);

    char buf[4096] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    assert(n > 0);
    buf[n] = '\0';

    // Response should be 400 Bad Request
    assert(strstr(buf, "400") != nullptr || strstr(buf, "400 Bad Request") != nullptr);
    close(fd);
}

void test_e2e_keepalive_two_requests() {
    HttpTestFixture f;

    // Raw socket — keep one connection open for two requests
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(f.port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    assert(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) >= 0);

    // First request
    const char* req1 =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    assert(write(fd, req1, strlen(req1)) > 0);
    char buf1[4096] = {};
    ssize_t n1 = read(fd, buf1, sizeof(buf1) - 1);
    assert(n1 > 0);
    buf1[n1] = '\0';
    assert(strstr(buf1, "200 OK") != nullptr);

    // Second request on same fd (keep-alive)
    const char* req2 =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "world";
    assert(write(fd, req2, strlen(req2)) > 0);
    char buf2[4096] = {};
    ssize_t n2 = read(fd, buf2, sizeof(buf2) - 1);
    assert(n2 > 0);
    buf2[n2] = '\0';
    assert(strstr(buf2, "200 OK") != nullptr);

    close(fd);
}

void test_e2e_named_route_params() {
    HttpTestFixture f;

    HttpClient client(nullptr);
    std::string url = "http://127.0.0.1:" + std::to_string(f.port) + "/echo/test-user";
    auto future = client.post(url, make_body("ignored"));

    auto result = future.get();
    assert(result.has_value());
    const auto& resp = result.value();
    std::string resp_str(resp.begin(), resp.end());
    // The echo route with :name echoes the name, not the body
    assert(resp_str == "test-user");
}

int main() {
    test_e2e_post_echo_actor();
    test_e2e_missing_route_404();
    test_e2e_actor_timeout_504();
    test_e2e_invalid_http_400();
    test_e2e_keepalive_two_requests();
    test_e2e_named_route_params();
    return 0;
}
```

- [ ] **Step 2: Verify build and run**

```bash
ninja -C build && ctest --output-on-failure -R test_http_server
```

Expected: test_http_server built and passes all 6 assertions.
Note: `test_e2e_actor_timeout_504` has a 5-second client-side timeout from RpcFuture. The integration tests may take ~7-8 seconds total to run due to this test. Consider marking it with `TIMEOUT 15` in CMake:

```cmake
set_tests_properties(test_http_server PROPERTIES TIMEOUT 15)
```

---

## Task 8: Add Test Targets to Build System

**Files:**
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add 3 test targets**

Append to `tests/CMakeLists.txt`:

```cmake
# =============================================================================
# HTTP tests
# =============================================================================
add_executable(test_http_parser net/test_http_parser.cpp)
target_link_libraries(test_http_parser hpactor)
add_test(NAME test_http_parser COMMAND test_http_parser)

add_executable(test_http_serializer net/test_http_serializer.cpp)
target_link_libraries(test_http_serializer hpactor)
add_test(NAME test_http_serializer COMMAND test_http_serializer)

add_executable(test_http_server net/test_http_server.cpp)
target_link_libraries(test_http_server hpactor)
add_test(NAME test_http_server COMMAND test_http_server)
set_tests_properties(test_http_server PROPERTIES TIMEOUT 15)
```

- [ ] **Step 2: Full build and test suite**

```bash
cmake -S . -B build -GNinja && ninja -C build && ctest --output-on-failure
```

Expected: All existing 62 tests + 3 new HTTP tests = 65 tests passing.

---

## Summary

| Task | Files | Test Count |
|------|-------|-----------|
| 1. Add HTTP error codes | `types.hpp` | — |
| 2. HttpParser response-mode | `http_parser.hpp`, `http_parser.cpp` | — |
| 3. HttpServer blocking listen() | `http_server.hpp`, `http_server.cpp` | — |
| 4. HttpClient build-out | `http_client.cpp` | — |
| 5. HttpParser unit tests | `test_http_parser.cpp` | 8 |
| 6. HttpSerializer unit tests | `test_http_serializer.cpp` | 5 |
| 7. HttpServer integration tests | `test_http_server.cpp` | 6 |
| 8. Build system integration | `tests/CMakeLists.txt` | — |

**Total: 19 new tests, 3 new test files, 6 modified files, 65 total tests at completion.**
