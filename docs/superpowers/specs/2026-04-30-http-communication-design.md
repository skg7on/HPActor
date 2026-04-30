# HTTP Communication Design Specification

**Date:** 2026-04-30
**Status:** Draft
**Author:** HPActor Team

---

## Overview

Add an HTTP communication subsystem on top of the existing EventLoop/StreamBuffer infrastructure. The subsystem provides HTTP ingress (external clients calling actors via REST), HTTP egress (actors calling external services), and as a natural consequence, actor-to-actor communication over HTTP.

## Goals

- **HttpServer**: Accept HTTP/1.1 connections, route requests to actors, collect replies, format HTTP responses
- **HttpClient**: Async HTTP requests to external URLs, keep-alive connection pooling, retry with backoff
- **HttpSerializer**: JSON ↔ protobuf conversion with Content-Type negotiation
- **HTTP/1.1 parser**: Embed llhttp for production-grade parsing, integrated with StreamBuffer
- **Actors unchanged**: Actors continue handling `TypedMessage` — no HTTP awareness leaks into actor code
- **Reuse existing infrastructure**: EventLoop for async I/O, StreamBuffer for accumulation, RpcFuture pattern for async results
- **No new external dependencies** beyond vendored llhttp (MIT, single `.c`/`.h`)

## Non-Goals

- HTTP/2 or HTTP/3 (QUIC) — may be added later as alternative parsers
- WebSocket upgrade — future phase
- Server-Sent Events — future phase
- gRPC — protobuf over HTTP is supported via content negotiation, but gRPC-specific framing is out of scope
- HTTPS/TLS termination at the HTTP layer — TLS is handled by the existing `TlsContext`/`TlsConnection`; HttpServer accepts plain connections; for TLS, place a reverse proxy or extend later
- HTTP caching headers (ETag, If-None-Match) — may be added later

---

## Architecture

### Component Hierarchy

```
ActorSystem
    ├── HttpServer (1:1 with ActorSystem)
    │   ├── Route registry (HttpMethod + pattern → builder)
    │   ├── Per-connection parser state (llhttp + StreamBuffer)
    │   └── Pending response map (request_id → connection)
    │
    ├── HttpClient (1:1 with ActorSystem)
    │   ├── Per-host connection pool (keep-alive)
    │   ├── Pending request map (request_id → RpcFuture)
    │   └── Retry state machine
    │
    └── HttpSerializer (stateless, shared)
        ├── JSON → protobuf (ingress)
        └── protobuf → JSON (egress)

ActorContext
    ├── http_get(url) → RpcFuture<HttpResponse>
    ├── http_post(url, body) → RpcFuture<HttpResponse>
    └── http_request(method, url, headers, body) → RpcFuture<HttpResponse>
```

---

## HTTP/1.1 Parser (llhttp Integration)

### Design

llhttp is a streaming parser that calls user-provided callbacks as it progresses through the HTTP message. The integration wraps llhttp in a `HttpParser` adapter that bridges between llhttp's C callbacks and HPActor's `StreamBuffer` / `HttpRequest` / `HttpResponse` types.

### llhttp Callback Mapping

| llhttp callback | Adapter action |
|----------------|---------------|
| `on_message_begin` | Initialize `HttpRequest` or `HttpResponse` struct |
| `on_url` | Accumulate URL into `HttpRequest::path` |
| `on_method` | Set `HttpRequest::method` from method string |
| `on_header_field` | Buffer the header name |
| `on_header_value` | Buffer the header value, push pair on complete |
| `on_headers_complete` | Record HTTP version, check for Upgrade/Connection headers |
| `on_body` | Append chunk to `StreamBuffer` (zero-copy when possible) |
| `on_message_complete` | Mark parse complete, invoke user callback |

### Parser State

```cpp
enum class HttpParseState {
    Idle,           // Waiting for new message
    ParsingRequest, // Parsing incoming request
    ParsingResponse,// Parsing incoming response
    Complete,       // Message fully parsed, user notified
    Error,          // Parse error, error code available
};

struct HttpParserState {
    llhttp_t parser;
    llhttp_settings_t settings;
    HttpParseState state = HttpParseState::Idle;
    StreamBuffer url_buf;
    StreamBuffer header_name_buf;
    StreamBuffer header_value_buf;
    std::vector<HttpHeader> headers;
    HttpMethod method;
    int http_major;
    int http_minor;
    optional<llhttp_errno_t> error;
};
```

### StreamBuffer Integration

```
Raw socket bytes → StreamBuffer → llhttp_execute() → callbacks → HttpRequest
```

`StreamBuffer` provides the read accumulation. llhttp's `on_body` callback receives pointers into the StreamBuffer's data — no copy for the body path. Headers and URL are copied into dedicated buffers since they're small.

### Upgrade Detection

llhttp exposes `llhttp_get_upgrade()` after `on_headers_complete`. If true, the parser stops consuming and the caller can take over the connection for WebSocket or other upgraded protocols. The initial implementation returns `501 Not Implemented` for upgrade requests; WebSocket support is a future phase.

---

## HttpServer

### Interface

```cpp
class HttpServer {
public:
    explicit HttpServer(ActorSystem* system);
    ~HttpServer();

    // Lifecycle
    void listen(uint16_t port, std::string host = "0.0.0.0");
    void stop();

    // Route registration (must be called before listen())
    using MessageBuilder = std::function<TypedMessage(const HttpRequest&)>;
    void route(HttpMethod method, std::string path_pattern, MessageBuilder builder);

    // Configuration
    void set_reply_timeout(std::chrono::milliseconds timeout);
    void set_max_connections(size_t max);
    void set_keepalive_timeout(std::chrono::milliseconds timeout);
    void set_max_request_size(size_t max_bytes);

    // Accessors
    uint16_t port() const;
    bool is_running() const;

private:
    void on_accept(int client_fd);
    void on_read(int client_fd);
    void on_write_complete(int client_fd);
    void on_parse_complete(int client_fd, HttpRequest&& request);
    void on_reply(int client_fd, uint64_t request_id, TypedMessage&& msg);
    void send_error_response(int client_fd, HttpStatusCode code, std::string message);
    void close_connection(int client_fd);

    struct PendingRequest {
        uint64_t request_id;
        int client_fd;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    struct ConnectionState {
        int fd;
        HttpParserState parser;
        StreamBuffer read_buf;
        std::vector<bytes> write_queue;  // queued responses
        bool keepalive = true;
        uint64_t next_request_id = 1;
    };

    ActorSystem* system_;
    EventLoop loop_;
    Acceptor acceptor_;
    std::unique_ptr<HttpSerializer> serializer_;
    RouteRegistry routes_;
    std::unordered_map<int, ConnectionState> connections_;
    std::unordered_map<uint64_t, PendingRequest> pending_replies_;
    std::chrono::milliseconds reply_timeout_{5000};
    std::chrono::milliseconds keepalive_timeout_{30000};
    size_t max_connections_{1000};
    size_t max_request_size_{1048576}; // 1MB
    uint64_t next_request_id_{1};
};
```

### Route Registry

```cpp
class RouteRegistry {
public:
    struct Route {
        HttpMethod method;
        std::string pattern;  // e.g. "/actors/:type/:id/:action"
        HttpServer::MessageBuilder builder;
        int priority = 0;     // Lower = higher priority
    };

    void add(HttpMethod method, std::string pattern,
             HttpServer::MessageBuilder builder, int priority = 0);
    const Route* match(HttpMethod method, const std::string& path) const;

private:
    // Ordered by priority, then insertion order
    std::vector<Route> routes_;
};
```

### Path Pattern Matching

Pattern syntax:

| Token | Meaning | Example match |
|-------|---------|---------------|
| Literal | Exact segment match | `/actors` |
| `:name` | Named parameter (single segment) | `:id` matches `42` |
| `*` | Wildcard (single segment, unnamed) | `*` matches `anything` |
| `*name` | Wildcard (multi-segment, named) | `*path` matches `a/b/c` |

Matching algorithm:

1. Split both pattern and path by `/`
2. Walk segments left to right
3. If pattern segment is literal, require exact match (case-sensitive)
4. If pattern segment is `:name`, match any single non-empty segment, capture value
5. If pattern segment is `*name` (last segment in pattern), match remaining path segments, capture value
6. If segment count differs and no trailing wildcard, no match
7. Return matched route or nullptr

### Connection Lifecycle

```
[EventLoop accept] → ConnectionState created (State: Active)
    → Bytes arrive → llhttp_execute()
        → on_message_complete → route match → deliver to actor
            → Response ready → write to socket → (State: Active, keepalive)
            → Actor timeout → write 504 → close or (State: Active)
        → on_headers_complete with Connection: close → (State: Draining)
    → Write complete → if Draining, close; else remain Active
    → Keepalive timeout → close
    → Client closes → EventLoop EOF → cleanup ConnectionState
```

### Actor Reply Routing

The HttpServer exposes a reply-collector address. When building the `TypedMessage` for the actor, the `sender` field is set to this address. The actor's `context()->reply()` routes back to the HttpServer.

To correlate replies with HTTP connections, each outbound message carries:
- `request_id` — unique per-connection request counter
- `connection_fd` — stored in `pending_replies_` map keyed by `request_id`

When a reply arrives:
1. Look up `pending_replies_[request_id]` → get `connection_fd`
2. Find `connections_[connection_fd]` → get connection state
3. Convert reply TypedMessage → HttpResponse via HttpSerializer
4. Serialize HTTP response bytes
5. Queue write on the connection's EventLoop registration
6. Remove from `pending_replies_`

---

## HttpClient

### Interface

```cpp
class HttpClient {
public:
    explicit HttpClient(EventLoop* loop);
    ~HttpClient();

    // Core: async HTTP request
    RpcFuture<HttpResponse> request(HttpMethod method,
                                     const std::string& url,
                                     std::vector<HttpHeader> headers = {},
                                     bytes body = {});

    // Convenience
    RpcFuture<HttpResponse> get(const std::string& url,
                                 std::vector<HttpHeader> headers = {});
    RpcFuture<HttpResponse> post(const std::string& url,
                                  bytes body,
                                  std::vector<HttpHeader> headers = {});
    RpcFuture<HttpResponse> put(const std::string& url,
                                 bytes body,
                                 std::vector<HttpHeader> headers = {});
    RpcFuture<HttpResponse> del(const std::string& url,
                                 std::vector<HttpHeader> headers = {});

    // Lifecycle
    void abort();  // Cancel all in-flight requests

    // Configuration
    void set_default_timeout(std::chrono::milliseconds timeout);
    void set_max_retries(int retries);
    void set_keepalive(bool enable);
    void set_max_connections_per_host(size_t max);

private:
    struct ParsedUrl {
        std::string scheme;   // "http" or "https"
        std::string host;
        uint16_t port;
        std::string path;     // includes query string
    };

    ParsedUrl parse_url(const std::string& url);
    std::shared_ptr<ConnectionPool> get_pool(const ParsedUrl& url);
    void send_request(uint64_t request_id);
    void on_response(uint64_t request_id, const HttpResponse& response);
    void on_error(uint64_t request_id, error err);
    void schedule_retry(uint64_t request_id);

    struct PendingHttpCall {
        uint64_t request_id;
        HttpMethod method;
        std::string host;
        uint16_t port;
        std::string path;
        std::vector<HttpHeader> headers;
        bytes body;
        int retry_count = 0;
        int max_retries = 3;
        std::chrono::milliseconds timeout{5000};
        std::chrono::steady_clock::time_point enqueued_at;
        std::promise<result<HttpResponse>> promise;
    };

    EventLoop* loop_;
    std::unordered_map<std::string, std::shared_ptr<HttpKeepAlivePool>> pools_;
    std::unordered_map<uint64_t, std::unique_ptr<PendingHttpCall>> pending_;
    mutable std::mutex mutex_;
    std::chrono::milliseconds default_timeout_{5000};
    int max_retries_{3};
    bool keepalive_enabled_{true};
    size_t max_connections_per_host_{4};
    uint64_t next_request_id_{1};
};
```

### URL Parsing

The `parse_url()` function extracts scheme, host, port, and path from a URL string:

```
"http://example.com:8080/api/v1/items?id=42"
→ scheme: "http", host: "example.com", port: 8080, path: "/api/v1/items?id=42"

"https://api.example.com/data"
→ scheme: "https", host: "api.example.com", port: 443, path: "/data"
```

Default ports: HTTP → 80, HTTPS → 443.

### Connection Pooling

Per-destination connection pools (keyed by `host:port`) maintain keep-alive connections. Pool behavior:

- **Acquire**: Return an idle connection from the pool, or create a new one (up to `max_connections_per_host`)
- **Release**: Return connection to idle pool after response completes (if `Connection: keep-alive`)
- **Eviction**: Close idle connections after `keepalive_timeout` (default 30s)
- **Backoff**: Exponential backoff on connection failure (100ms → 200ms → 400ms → ... → 10s max)

### Retry State Machine

```
[request()] → Pending (send to socket)
    → [response received] → Fulfill promise → DONE
    → [connection error] → Retry (increment counter, backoff delay, re-send)
        → [retries < max_retries] → Pending
        → [retries >= max_retries] → Reject promise with error → DONE
    → [timeout] → Retry
        → [retries < max_retries] → Pending
        → [retries >= max_retries] → Reject promise with timeout error → DONE
```

### Connection Establishment

```
[request(url)] → parse_url → get_pool(host:port)
    → pool->acquire() → idle connection? → use it
    → no idle connection, under limit? → connect() → TCP + optional TLS
    → all connections busy? → wait (with timeout) or create temporary connection
```

TLS is handled by the existing `TlsConnection` when the URL scheme is `https`. The HttpClient creates `TlsConnection` instances via the existing TLS infrastructure.

---

## HttpSerializer

### Interface

```cpp
class HttpSerializer {
public:
    // Ingress: HTTP request body → TypedMessage (for actor delivery)
    TypedMessage deserialize_request(const HttpRequest& req, TypeTag expected_tag);

    // Egress: TypedMessage → HTTP response body + Content-Type
    std::pair<bytes, std::string> serialize_response(const TypedMessage& msg,
                                                      const std::string& accept_header);

    // Egress: TypedMessage → HTTP request body + Content-Type (for HttpClient)
    std::pair<bytes, std::string> serialize_request(const TypedMessage& msg);

    // Egress: HTTP response body → TypedMessage (for HttpClient response parsing)
    TypedMessage deserialize_response(const HttpResponse& resp, TypeTag expected_tag);
};
```

### Content Negotiation Algorithm (Ingress)

```
Input: Content-Type header value, request body bytes, expected TypeTag
Output: TypedMessage

1. If Content-Type is "application/x-protobuf":
   a. Deserialize body using DefaultSerializer → protobuf message of expected TypeTag
   b. Return TypedMessage{tag=expected_tag, payload=deserialized_protobuf}

2. If Content-Type is "application/json" or absent:
   a. Parse body as JSON
   b. Look up JSON→protobuf mapping for expected TypeTag
   c. Convert JSON fields to protobuf fields
   d. Return TypedMessage{tag=expected_tag, payload=converted_protobuf}

3. If Content-Type is "text/plain":
   a. Wrap body in bytes payload
   b. Return TypedMessage{tag=expected_tag, payload=body_as_bytes}

4. Otherwise:
   a. Return error: unsupported Content-Type
```

### Content Negotiation Algorithm (Egress Response)

```
Input: TypedMessage, Accept header value
Output: pair<body bytes, Content-Type string>

1. Parse Accept header into ordered list of media types with quality weights
2. For each accepted type (in quality order):
   a. If "application/json" or "*/*":
      - Convert protobuf fields to JSON
      - Return {json_bytes, "application/json"}
   b. If "application/x-protobuf":
      - Serialize protobuf message with DefaultSerializer
      - Return {protobuf_bytes, "application/x-protobuf"}
3. Default: application/json
```

### JSON ↔ Protobuf Conversion

The HttpSerializer maintains a registry of TypeTag → protobuf descriptor mappings. For each known TypeTag, it knows:

- The protobuf message type
- Field names and types (from protobuf reflection or a hand-maintained mapping table)
- How to convert between JSON values and protobuf field types

The initial implementation uses a hand-maintained mapping table (similar to how `ActorTypeRegistry` maps actor type names to factory functions). Future optimization: use protobuf's `Descriptor` reflection if available.

---

## Type System

### New Types

```cpp
// types.hpp additions
enum class TypeTag : uint32_t {
    // ... existing tags ...
    HttpRequestTag  = 8,
    HttpResponseTag = 9,
    User = 100,
};

// http_types.hpp
enum class HttpMethod : uint8_t {
    GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS
};

enum class HttpStatusCode : uint16_t {
    OK = 200, Created = 201, Accepted = 202, NoContent = 204,
    BadRequest = 400, Unauthorized = 401, Forbidden = 403,
    NotFound = 404, MethodNotAllowed = 405, Conflict = 409,
    UnsupportedMedia = 415, TooManyRequests = 429,
    InternalError = 500, NotImplemented = 501,
    BadGateway = 502, ServiceUnavailable = 503, GatewayTimeout = 504,
};

struct HttpHeader { std::string name; std::string value; };

struct HttpRequest {
    HttpMethod method;
    std::string path;
    int http_major = 1;
    int http_minor = 1;
    std::vector<HttpHeader> headers;
    bytes body;
    std::unordered_map<std::string, std::string> path_params;
    std::unordered_map<std::string, std::string> query_params;

    std::optional<std::string> header(const std::string& name) const;
    std::optional<std::string> content_type() const;
};

struct HttpResponse {
    HttpStatusCode status_code = HttpStatusCode::OK;
    std::vector<HttpHeader> headers;
    bytes body;

    static HttpResponse ok(bytes body = {});
    static HttpResponse created(bytes body = {});
    static HttpResponse no_content();
    static HttpResponse not_found(bytes body = {});
    static HttpResponse error(HttpStatusCode code, std::string message);
};
```

### HttpResponse Serialization Format

```
HTTP/1.1 <status_code> <reason_phrase>\r\n
<header_name>: <header_value>\r\n
...\r\n
\r\n
<body_bytes>
```

### HttpRequest Serialization Format (for HttpClient outbound)

```
<METHOD> <path> HTTP/1.1\r\n
Host: <host>\r\n
<header_name>: <header_value>\r\n
...\r\n
\r\n
<body_bytes>
```

---

## ActorContext Integration

```cpp
// actor_context.hpp additions
class ActorContext {
public:
    // ... existing methods ...

    // HTTP egress — delegates to ActorSystem's HttpClient
    RpcFuture<HttpResponse> http_get(const std::string& url,
                                      std::vector<HttpHeader> headers = {});
    RpcFuture<HttpResponse> http_post(const std::string& url,
                                       bytes body,
                                       std::vector<HttpHeader> headers = {});
    RpcFuture<HttpResponse> http_request(HttpMethod method,
                                          const std::string& url,
                                          std::vector<HttpHeader> headers = {},
                                          bytes body = {});
};
```

These are thin wrappers that delegate to `ActorSystem::http_client()` → `HttpClient::request()`. They return `RpcFuture<HttpResponse>` using the same pattern as `ActorContext::rpc()`.

---

## Error Handling

### Error Codes

```cpp
namespace errors {
    constexpr uint32_t http_parse_error     = 2001;
    constexpr uint32_t http_connect_failed  = 2002;
    constexpr uint32_t http_timeout         = 2003;
    constexpr uint32_t http_too_many_retries = 2004;
    constexpr uint32_t http_unsupported_media = 2005;
    constexpr uint32_t http_no_route        = 2006;
    constexpr uint32_t http_request_too_large = 2007;
}
```

### Error to HTTP Status Mapping

```cpp
HttpStatusCode status_for_error(const error& err) {
    switch (err.code()) {
        case errors::actor_not_found: return HttpStatusCode::NotFound;
        case errors::mailbox_full:    return HttpStatusCode::ServiceUnavailable;
        case errors::timeout:         return HttpStatusCode::GatewayTimeout;
        case errors::http_no_route:   return HttpStatusCode::NotFound;
        case errors::http_unsupported_media: return HttpStatusCode::UnsupportedMedia;
        case errors::http_request_too_large: return HttpStatusCode::PayloadTooLarge;
        default:                      return HttpStatusCode::InternalError;
    }
}
```

---

## Thread Safety

| Component | Thread Model |
|-----------|-------------|
| HttpServer | I/O on EventLoop thread; route registration thread-safe before `listen()`; immutable after |
| HttpClient | I/O on EventLoop thread; `RpcFuture` is thread-safe (existing pattern); `request()` may be called from any thread |
| HttpSerializer | Stateless — callable from any thread |
| llhttp parser state | Per-connection, accessed only from the owning EventLoop thread |

---

## Files

| File | Purpose |
|------|---------|
| `include/hpactor/net/http_types.hpp` | HttpMethod, HttpStatusCode, HttpHeader, HttpRequest, HttpResponse |
| `include/hpactor/net/http_server.hpp` | HttpServer class declaration |
| `include/hpactor/net/http_client.hpp` | HttpClient class declaration |
| `include/hpactor/net/http_serializer.hpp` | HttpSerializer class declaration |
| `src/net/http_server.cpp` | HttpServer implementation |
| `src/net/http_client.cpp` | HttpClient implementation |
| `src/net/http_parser.cpp` | llhttp StreamBuffer adapter |
| `src/net/http_serializer.cpp` | HttpSerializer implementation |
| `third_party/llhttp/llhttp.h` | Vendored llhttp header |
| `third_party/llhttp/llhttp.c` | Vendored llhttp source |
| `tests/net/test_http_parser.cpp` | HTTP/1.1 parser unit tests |
| `tests/net/test_http_server.cpp` | HttpServer integration tests |
| `tests/net/test_http_client.cpp` | HttpClient integration tests |
| `tests/net/test_http_serializer.cpp` | Serialization round-trip tests |
