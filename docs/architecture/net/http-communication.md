# HTTP Communication

The HTTP subsystem enables actors to communicate through HTTP at system boundaries — external clients calling actors via REST APIs, actors calling external web services, and (as a natural consequence) actor-to-actor communication over HTTP when the network demands it.

## The Big Picture

The existing protobuf-over-TCP transport is efficient for actor-to-actor communication within a trusted network. HTTP addresses the boundaries:

- **Ingress (External → Actors)**: REST APIs, webhooks, browser clients, mobile apps
- **Egress (Actors → External)**: Third-party APIs, OAuth token exchange, webhook delivery
- **Cross-boundary (Actor ↔ Actor)**: Firewall traversal, load balancer integration, CDN caching

The core design principle: **actors remain transport-agnostic**. An actor handles `TypedMessage` and calls `context()->reply()` — it never knows whether the caller arrived via TCP, HTTP, or a local mailbox. The HTTP subsystem is a translation layer at the boundary.

```
                        ┌──────────────────────────────────────────────────┐
                        │                 ActorSystem                       │
                        │                                                   │
  HTTP Client ─────────►│  ┌──────────────────────┐                         │
  (REST, browser,       │  │   HTTPServerActor    │  DaemonActor            │
   webhook)             │  │   (ingress daemon)   │  ─ owns EventLoop       │
                        │  │                      │  ─ owns HTTPConnection[] │
                        │  └──┬───────────────────┘                         │
                        │     │ TypedMessage (via ActorContext::send)       │
                        │     ▼                                             │
                        │  ┌──────────────────────┐                         │
                        │  │   EventBasedActor    │                         │
                        │  │   (registered route) │                         │
                        │  │   (unchanged)        │                         │
                        │  └──┬───────────────────┘                         │
                        │     │ context()->reply()                          │
                        │     ▼                                             │
                        │  ┌──────────────────────┐                         │
                        │  │   ReplyAdapter       │                         │
                        │  │   (internal actor)   │                         │
                        │  └──┬───────────────────┘                         │
                        │     │ TypedMessage                              │
                        │     ▼                                             │
                        │  HTTPServerActor serializes → HTTP response       │
                        │                                                   │
  External API ◄────────│  ┌──────────────────────┐                         │
  (third-party HTTP)    │  │     HttpClient       │                         │
                        │  │     (egress)         │                         │
                        │  │  ─ owns HTTPConns    │                         │
                        │  └──────────────────────┘                         │
                        └──────────────────────────────────────────────────┘
```

## Why HTTP Is Not a Transport

The existing `Transport` interface models persistent connections with fire-and-forget semantics. HTTP has fundamentally different semantics:

| Property | Transport (TCP custom framing) | HTTP |
|----------|-------------------------------|------|
| Connection model | Persistent, connection-per-node pool | Ephemeral or pooled, connection-per-request logically |
| Message exchange | Unidirectional fire-and-forget | Request-response correlated |
| Addressing | ActorAddress embedded in frame | URL path + method + headers |
| Serialization | Single format (protobuf) per connection | Content-Type negotiation per request |
| Error model | Frame-level errors | HTTP status codes (4xx, 5xx) |
| Intermediate nodes | Transparent TCP proxies | HTTP proxies, CDNs, load balancers with caching |

Forcing HTTP into the Transport interface would strip away methods, status codes, headers, and content negotiation — the very features that make HTTP valuable. HTTP needs its own abstraction layer that complements Transport, not replaces it.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                      HTTP Subsystem                                │
│                                                                    │
│  ┌──────────────────────┐    ┌──────────────────────┐             │
│  │   HTTPServerActor    │    │     HttpClient       │             │
│  │   (DaemonActor)      │    │   (EventLoop comp.)  │             │
│  │                      │    │                      │             │
│  │  Route registry      │    │  Connection pool     │             │
│  │  Request parsing     │    │  Request builder     │             │
│  │  Reply routing       │    │  Response parser     │             │
│  │  Timeout/errors      │    │  Timeout/retry       │             │
│  │  TypedMessage conv.  │    │  RpcFuture<StreamBuf>│             │
│  └──────────┬───────────┘    └──────────┬───────────┘             │
│             │                           │                          │
│             └─────────────┬─────────────┘                          │
│                           │                                        │
│                ┌──────────▼───────────┐                            │
│                │   HTTPConnection     │  extends Connection        │
│                │                      │                            │
│                │  send/recv framing   │                            │
│                │  llhttp integration  │                            │
│                │  keep-alive mgmt     │                            │
│                └──────────┬───────────┘                            │
│                           │                                        │
│                ┌──────────▼───────────┐                            │
│                │   HttpSerializer     │                            │
│                │                      │                            │
│                │  JSON ↔ protobuf     │                            │
│                │  Content-Type        │                            │
│                │  negotiation         │                            │
│                └──────────┬───────────┘                            │
│                           │                                        │
│                ┌──────────▼───────────┐                            │
│                │   HTTP/1.1           │                            │
│                │   Parser (llhttp)    │                            │
│                └──────────────────────┘                            │
└──────────────────────────────────────────────────────────────────┘
```

## 1. Config-Controlled HTTP Features

HTTP features are gated by `Config` flags, following the existing `enable_network` pattern. When disabled, no HTTP infrastructure is created.

```cpp
struct Config {
    // ... existing fields ...

    // HTTP subsystem (requires enable_network = true)
    bool enable_http_server = false;   // Ingress: REST API for external callers
    bool enable_http_client = false;   // Egress: actors calling external HTTP services

    // HTTP server configuration
    uint16_t http_port = 8080;
    std::string http_bind_host = "0.0.0.0";
    size_t http_max_connections = 1000;
    size_t http_max_request_size = 1048576;  // 1 MiB
    std::chrono::milliseconds http_reply_timeout{5000};
};
```

In `ActorSystem` constructor:

```cpp
if (config.enable_network && config.enable_http_server) {
    http_server_ = system().spawn<HTTPServerActor>(
        config.http_bind_host, config.http_port);
    http_server_->set_max_connections(config.http_max_connections);
    http_server_->set_reply_timeout(config.http_reply_timeout);
}

if (config.enable_network && config.enable_http_client) {
    http_client_ = std::make_unique<net::HttpClient>(network_loop_.get());
}
```

## 2. HTTPConnection — Common HTTP Framing Layer

`HTTPConnection` extends the existing `Connection` base class, providing a shared HTTP framing layer used by both `HttpServer` and `HttpClient`. It mirrors the pattern of `WireFrameConnection`: a protocol-specific `Connection` subclass that owns an llhttp parser and handles the read/write lifecycle.

```
Connection (transport.hpp)
    ├── WireFrameConnection   (frame.hpp)       — HPAC magic + length framing
    └── HTTPConnection        (http_connection) — HTTP/1.1 request/response framing
```

### 2.1 Responsibilities

- **Read side**: Accumulate incoming bytes, feed to llhttp parser, invoke callback on complete HTTP message (request or response depending on mode).
- **Write side**: Accept a `StreamBuffer` (the HTTP wire bytes), write to socket via EventLoop async I/O, queue writes when socket is busy.
- **Keep-alive**: Track `Connection: keep-alive` / `Connection: close` headers; expose `should_keep_alive()`.
- **Error handling**: Parse errors → close connection + invoke error callback. Write errors → close connection.

### 2.2 Interface

```cpp
// HTTPConnectionMode — configures llhttp for request or response parsing
enum class HTTPConnectionMode { Server, Client };

class HTTPConnection : public Connection,
                       public std::enable_shared_from_this<HTTPConnection> {
  public:
    // Callbacks
    using RequestHandler = std::function<void(HTTPConnection*, HttpRequest&&)>;
    using ResponseHandler = std::function<void(HTTPConnection*, int status_code,
                                                std::vector<HttpHeader>, StreamBuffer)>;
    using ErrorHandler = std::function<void(HTTPConnection*, HttpStatusCode, const std::string&)>;

    static std::shared_ptr<HTTPConnection>
    create(int fd, EndPoint local_ep, EndPoint remote_ep,
           EventLoop* loop, HTTPConnectionMode mode);

    ~HTTPConnection();

    // Set callbacks (before first read)
    void set_request_handler(RequestHandler h);
    void set_response_handler(ResponseHandler h);
    void set_error_handler(ErrorHandler h);

    // Send HTTP response bytes (server) or request bytes (client)
    void send_response(HttpStatusCode code, std::vector<HttpHeader> headers,
                       StreamBuffer body);
    void send_request(HttpMethod method, const std::string& path,
                      std::vector<HttpHeader> headers, StreamBuffer body);
    void send_raw(StreamBuffer data);

    // Connection overrides
    void send(const StreamBuffer& data) override;
    void close() override;
    void handle_read() override;
    void handle_send_completion(int result) override;

    // State
    bool should_keep_alive() const;
    HTTPConnectionMode mode() const { return mode_; }

  private:
    HTTPConnection(int fd, EndPoint local_ep, EndPoint remote_ep,
                   EventLoop* loop, HTTPConnectionMode mode);

    void parse_incoming(StreamBuffer& data);
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
```

### 2.3 Read Flow (Server Mode — parsing incoming HTTP requests)

```
EventLoop → handle_read()
    → read(fd, buf, sizeof(buf))
    → read_buf_.append(buf, n)
    → parser_->execute(read_buf_.span())
    → llhttp callbacks fire:
        on_url, on_header_field, on_header_value,
        on_headers_complete, on_body, on_message_complete
    → on_message_complete:
        → request_handler_(this, std::move(parsed_request))
    → if parser state == Complete: read_buf_.clear()
```

### 2.4 Write Flow (Server Mode — sending HTTP response)

```
send_response(code, headers, body)
    → Build HTTP/1.1 wire bytes:
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 42\r\n"
        "\r\n"
        <body bytes>
    → If not currently sending:
        → write(fd, wire) (non-blocking, via EventLoop async send)
    → If currently sending:
        → write_queue_.push_back(wire)
    → handle_send_completion():
        → flush next from write_queue_
```

### 2.5 HTTPConnection vs WireFrameConnection — Shared Pattern

| Aspect | WireFrameConnection | HTTPConnection |
|--------|-------------------|----------------|
| Base class | `Connection` | `Connection` |
| Framing | Magic "HPAC" + 4-byte BE length | llhttp state machine |
| Read buffer | `StreamBuffer read_buffer_` | `StreamBuffer read_buf_` |
| Write buffer | `StreamBuffer write_buffer_` | `StreamBuffer write_buf_` |
| Write queue | Implicit (single frame at a time) | `std::vector<StreamBuffer> write_queue_` |
| Callback | `frame_handler` (StreamBuffer) | `RequestHandler` / `ResponseHandler` |
| Async send | `is_sending_` + `flush_write_buffer()` | `is_sending_` + `flush_write_queue()` |

## 3. HTTPServerActor — DaemonActor-Based Ingress

`HTTPServerActor` replaces the current `HttpServer` class. It extends `DaemonActor` (from the DispatchPolicy architecture), which means:

- **Unified supervision**: The server is in the supervision tree. If it crashes, a supervisor can restart it.
- **Unified messaging**: Routes convert HTTP requests to `TypedMessage` and deliver via `context()->send()` — standard actor messaging.
- **Isolation**: Its `DedicatedThread` dispatch policy ensures the accept/read/write loop never starves cooperative M:N workers.

### 3.1 Class Design

```cpp
class HTTPServerActor : public DaemonActor {
  public:
    using MessageBuilder =
        std::function<std::pair<ActorAddress, TypedMessage>(const HttpRequest&)>;

    HTTPServerActor(ActorContext* ctx, ActorSystem& sys,
                    const std::string& bind_host, uint16_t port);

    // Route registration (must be called before listen starts, typically in ctor)
    void route(HttpMethod method, std::string path_pattern,
               MessageBuilder builder, int priority = 0);
    void route(HttpMethod method, std::string path_pattern,
               ActorAddr target);

    // Configuration
    void set_reply_timeout(std::chrono::milliseconds t);
    void set_max_connections(size_t max);
    void set_max_request_size(size_t max_bytes);

    // DaemonActor override — this is the accept/process loop
    bool run_once() override;

  protected:
    void on_daemon_start() override;   // Create listen socket, bind, register with EventLoop
    void on_daemon_stop() override;    // Close all connections, close listen socket

  private:
    void on_accept();                  // Accept new connections
    void on_request(HTTPConnection* conn, HttpRequest&& req);  // Parse complete → route
    void on_reply(TypedMessage&& msg);                        // Actor reply → HTTP response
    void on_error(HTTPConnection* conn, HttpStatusCode code, const std::string& msg);
    void on_timeout(uint64_t request_id);
    void close_connection(HTTPConnection* conn);

    // Owned EventLoop (runs on the DaemonActor's dedicated thread)
    EventLoop loop_;

    // Route registry
    RouteRegistry routes_;

    // Serializer
    std::unique_ptr<HttpSerializer> serializer_;

    // Active connections: conn_ptr → HTTPConnection
    std::unordered_map<HTTPConnection*, std::shared_ptr<HTTPConnection>> connections_;

    // Pending replies: request_id → (HTTPConnection*, enqueued_at)
    struct PendingReply {
        uint64_t request_id;
        HTTPConnection* conn;
        std::chrono::steady_clock::time_point enqueued_at;
    };
    std::unordered_map<uint64_t, std::unique_ptr<PendingReply>> pending_replies_;

    // Reply adapter — internal EventBasedActor that receives replies
    Actor reply_adapter_;

    // Configuration
    std::string bind_host_;
    uint16_t port_;
    int listen_fd_ = -1;
    std::chrono::milliseconds reply_timeout_{5000};
    size_t max_connections_{1000};
    size_t max_request_size_{1048576};
    uint64_t next_request_id_{1};
};
```

### 3.2 End-to-End Ingress Flow

```
1. DaemonActor::on_activate()
   → on_daemon_start()
     → socket() + bind() + listen()
     → loop_.add_fd(listen_fd_, Event::Read)
     → loop_.set_read_handler(listen_fd_, [this]{ on_accept(); })

2. run_once() loop (called on dedicated thread):
   → loop_.wait(poll_timeout)
   → loop_.process_completions()
     → on_accept(): accept client, create HTTPConnection(Server mode)
     → HTTPConnection::handle_read(): parse request via llhttp
     → on_request(conn, req):
       → Match route: routes_.match(req.method, req.path, req)
       → Build TypedMessage via builder
       → context()->send(target, typed_msg)
       → Record PendingReply{request_id, conn}

3. Target actor processes message:
   → context()->reply(response_msg)
   → ReplyAdapter receives the reply
   → on_reply(reply_msg):
     → Look up PendingReply by request_id
     → HttpSerializer::serialize_response(reply_msg) → HTTP wire bytes
     → conn->send_response(200, headers, body)
     → If !conn->should_keep_alive(): close_connection(conn)

4. Timeout:
   → loop_.run_after() fires → on_timeout(request_id)
   → conn->send_response(504, ...)
   → close_connection(conn)
```

### 3.3 Route Convenience Overload

```cpp
// Simple overload: auto-wrap target actor address as a pass-through builder.
// The HttpRequest body is deserialized and sent directly to the target actor.
void HTTPServerActor::route(HttpMethod method, std::string path_pattern,
                             ActorAddr target) {
    route(method, std::move(path_pattern),
          [target, this](const HttpRequest& req) -> std::pair<ActorAddress, TypedMessage> {
              // Use HttpSerializer to convert HTTP body → TypedMessage
              auto result = serializer_->deserialize_request(req, TypeTag::User);
              if (!result.has_value()) {
                  // Error — handled by caller
                  return {invalid_actor_addr, TypedMessage{}};
              }
              return {target, std::move(result.value())};
          });
}
```

## 4. HttpClient — Egress (Largely Unchanged Structure)

`HttpClient` remains an EventLoop-integrated component (not an actor). Its role is outbound: actors call external HTTP services. It uses `HTTPConnection` instances internally for connection management.

### 4.1 Key Refinements

- Uses `HTTPConnection` in `Client` mode for framing — replaces the current raw `read()`/`write()` calls in `request()`.
- Keep-alive connection pool: reuse `HTTPConnection` instances per destination host.
- `RpcFuture<StreamBuffer>` return type preserved; the caller blocks on the future (intended for use from `BlockingActor` or non-actor contexts).

### 4.2 Interface (Refined)

```cpp
class HttpClient {
  public:
    explicit HttpClient(EventLoop* loop);
    ~HttpClient();

    RpcFuture<StreamBuffer> request(HttpMethod method, const std::string& url,
                                     std::vector<HttpHeader> headers = {},
                                     StreamBuffer body = {});
    RpcFuture<StreamBuffer> get(const std::string& url,
                                 std::vector<HttpHeader> headers = {});
    RpcFuture<StreamBuffer> post(const std::string& url, StreamBuffer body,
                                  std::vector<HttpHeader> headers = {});
    RpcFuture<StreamBuffer> put(const std::string& url, StreamBuffer body,
                                 std::vector<HttpHeader> headers = {});
    RpcFuture<StreamBuffer> del(const std::string& url,
                                 std::vector<HttpHeader> headers = {});

    void abort();

    // Configuration
    void set_default_timeout(std::chrono::milliseconds timeout);
    void set_max_retries(int retries);
    void set_keepalive(bool enable);
    void set_max_connections_per_host(size_t max);

  private:
    std::shared_ptr<HTTPConnection> get_connection(const ParsedUrl& target);

    EventLoop* loop_;
    std::unordered_map<std::string, std::vector<std::shared_ptr<HTTPConnection>>> pool_;
    std::mutex mutex_;
    std::chrono::milliseconds default_timeout_{5000};
    int max_retries_{3};
    bool keepalive_enabled_{true};
    size_t max_conns_per_host_{4};
};
```

## 5. Error Mapping

| Actor/system error | HTTP status |
|-------------------|-------------|
| Actor not found | 404 Not Found |
| Mailbox full | 503 Service Unavailable |
| Timeout (no reply) | 504 Gateway Timeout |
| Serialization failure | 400 Bad Request |
| No matching route | 404 Not Found |
| Unsupported Content-Type | 415 Unsupported Media Type |
| Internal error | 500 Internal Server Error |

## 6. Relationship to Existing Components

| Existing Component | HTTP Equivalent | Relationship |
|-------------------|----------------|-------------|
| `Acceptor` | `HTTPServerActor::on_accept()` | Same EventLoop async-accept pattern; now inside DaemonActor |
| `RpcChannel` | `HttpClient` | Same pending-call map + timeout + retry |
| `DefaultSerializer` | `HttpSerializer` | Same pluggable encode/decode, adds JSON |
| `ConnectionPool` | `HttpClient::pool_` | Keep-alive connection reuse per host |
| `WireFrameConnection` | `HTTPConnection` | Same pattern: `Connection` subclass with framing |
| `EventLoop` | `EventLoop` (owned by HTTPServerActor) | Dedicated EventLoop on the daemon thread |

## 7. Design Constraints

- **No new external dependencies** except vendored llhttp (MIT, single `.c`/`.h`)
- **Actors are unchanged** — no HTTP-specific actor base classes, no HTTP awareness in actor code
- **`-fno-exceptions`, `-fno-rtti`** — error handling via `error` type and result types, not exceptions
- **HTTP/1.1 only** for the initial implementation; HTTP/2 and WebSocket are future extensions
- **JSON is the default wire format** for HTTP; protobuf binary is an opt-in optimization
- **HTTPServerActor IS a DaemonActor** — it runs on a dedicated OS thread, participates in the supervision tree, and communicates via standard `context()->send()` / `context()->reply()`

## 8. Files

| File | Purpose |
|------|---------|
| `include/hpactor/net/http_types.hpp` | HttpMethod, HttpStatusCode, HttpHeader, HttpRequest, HttpResponse |
| `include/hpactor/net/http_connection.hpp` | **NEW** HTTPConnection — shared Connection subclass for HTTP framing |
| `include/hpactor/net/http_server.hpp` | HTTPServerActor — DaemonActor-based ingress (replaces old HttpServer) |
| `include/hpactor/net/http_client.hpp` | HttpClient — egress (refined to use HTTPConnection) |
| `include/hpactor/net/http_serializer.hpp` | HttpSerializer — JSON ↔ protobuf with content negotiation |
| `src/net/http_connection.cpp` | **NEW** HTTPConnection implementation |
| `src/net/http_server.cpp` | HTTPServerActor implementation (rewritten) |
| `src/net/http_client.cpp` | HttpClient implementation (refined to use HTTPConnection) |
| `src/net/http_parser.cpp` | llhttp StreamBuffer adapter |
| `src/net/http_serializer.cpp` | HttpSerializer implementation |
| `third_party/llhttp/` | Vendored llhttp source |
