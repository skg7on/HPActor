# HTTP Communication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement HTTP communication subsystem — HttpServer for ingress (HTTP → actors), HttpClient for egress (actors → HTTP), HttpSerializer for JSON ↔ protobuf conversion, with llhttp for production-grade HTTP/1.1 parsing.

**Architecture:** HttpServer is an EventLoop-integrated component (mirrors Acceptor), HttpClient follows the RpcChannel pattern, HttpSerializer is stateless and shared. Actors remain unchanged — all HTTP translation happens at the boundary.

**Tech Stack:** C++20, no exceptions, no RTTI, existing EventLoop/StreamBuffer/Acceptor/RpcChannel infrastructure, vendored llhttp (MIT, single .c/.h).

---

## File Structure

```
include/hpactor/net/
    ├── http_types.hpp          # HttpMethod, HttpStatusCode, HttpHeader, HttpRequest, HttpResponse
    ├── http_server.hpp          # HttpServer class declaration
    ├── http_client.hpp          # HttpClient class declaration
    └── http_serializer.hpp      # HttpSerializer class declaration

include/hpactor/types/
    └── types.hpp                # Add HttpRequestTag=8, HttpResponseTag=9 to TypeTag enum

include/hpactor/
    └── actor_context.hpp        # Add http_get(), http_post(), http_request() methods

src/net/
    ├── http_parser.cpp          # llhttp StreamBuffer adapter
    ├── http_server.cpp          # HttpServer implementation
    ├── http_client.cpp          # HttpClient implementation
    └── http_serializer.cpp      # HttpSerializer implementation

third_party/llhttp/
    ├── llhttp.h                 # Vendored header
    └── llhttp.c                 # Vendored source (linked into hpactor_lib)

tests/net/
    ├── test_http_parser.cpp     # HTTP/1.1 parser unit tests
    ├── test_http_server.cpp     # HttpServer integration tests
    ├── test_http_client.cpp     # HttpClient integration tests
    └── test_http_serializer.cpp # Serialization round-trip tests

CMakeLists.txt                   # Add llhttp source, test targets
```

---

## Task 1: Vendor llhttp and CMake Integration

**Files:**
- Create: `third_party/llhttp/llhttp.h`
- Create: `third_party/llhttp/llhttp.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Download and vendor llhttp**

Download llhttp from https://github.com/nodejs/llhttp (MIT license). The source is a single `.c`/`.h` pair generated from TypeScript — copy both into `third_party/llhttp/`.

```bash
# llhttp release v9.2.0 (stable, used by Node.js)
curl -L https://raw.githubusercontent.com/nodejs/llhttp/release/v9.2.0/src/llhttp.h \
     -o third_party/llhttp/llhttp.h
curl -L https://raw.githubusercontent.com/nodejs/llhttp/release/v9.2.0/src/llhttp.c \
     -o third_party/llhttp/llhttp.c
```

- [ ] **Step 2: Add llhttp to CMakeLists.txt**

Add `llhttp.c` to the hpactor_lib source list:

```cmake
# In the hpactor_lib target's source list
set(HTTP_PARSER_SOURCES
    third_party/llhttp/llhttp.c
    src/net/http_parser.cpp
)

# Add to hpactor_lib sources
target_sources(hpactor_lib PRIVATE ${HTTP_PARSER_SOURCES})

# Add third_party include path
target_include_directories(hpactor_lib PUBLIC third_party)
```

Ensure the existing compile flags are applied (`-fno-exceptions`, `-fno-rtti`, strict warnings). llhttp is pure C and compiles cleanly with these flags.

- [ ] **Step 3: Verify build**

```bash
cmake -S . -B build -GNinja && ninja -C build
```

Expected: PASS (no new code uses llhttp yet, just verify it compiles and links)

---

## Task 2: HTTP Core Types

**Files:**
- Create: `include/hpactor/net/http_types.hpp`

- [ ] **Step 1: Write the header**

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <hpactor/types/types.hpp>  // for bytes

namespace hpactor {
namespace net {

// ---------------------------------------------------------------------------
// HttpMethod
// ---------------------------------------------------------------------------
enum class HttpMethod : uint8_t {
    GET     = 0,
    POST    = 1,
    PUT     = 2,
    DELETE  = 3,
    PATCH   = 4,
    HEAD    = 5,
    OPTIONS = 6,
};

// ---------------------------------------------------------------------------
// HttpStatusCode
// ---------------------------------------------------------------------------
enum class HttpStatusCode : uint16_t {
    OK                  = 200,
    Created             = 201,
    Accepted            = 202,
    NoContent           = 204,
    BadRequest          = 400,
    Unauthorized        = 401,
    Forbidden           = 403,
    NotFound            = 404,
    MethodNotAllowed    = 405,
    Conflict            = 409,
    PayloadTooLarge     = 413,
    UnsupportedMedia    = 415,
    TooManyRequests     = 429,
    InternalError       = 500,
    NotImplemented      = 501,
    BadGateway          = 502,
    ServiceUnavailable  = 503,
    GatewayTimeout      = 504,
};

// ---------------------------------------------------------------------------
// HttpHeader
// ---------------------------------------------------------------------------
struct HttpHeader {
    std::string name;   // Lowercase
    std::string value;
};

// ---------------------------------------------------------------------------
// HttpRequest
// ---------------------------------------------------------------------------
struct HttpRequest {
    HttpMethod method = HttpMethod::GET;
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

// ---------------------------------------------------------------------------
// HttpResponse
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------
const char* to_string(HttpMethod method);
const char* reason_phrase(HttpStatusCode code);

} // namespace net
} // namespace hpactor
```

- [ ] **Step 2: Implement convenience methods (inline in header or in cpp)**

```cpp
// In http_types.hpp (inline)
inline std::optional<std::string>
HttpRequest::header(const std::string& name) const {
    for (const auto& h : headers) {
        if (h.name == name) return h.value;
    }
    return std::nullopt;
}

inline std::optional<std::string>
HttpRequest::content_type() const {
    return header("content-type");
}

// Static factories
inline HttpResponse HttpResponse::ok(bytes body) {
    return {HttpStatusCode::OK, {}, std::move(body)};
}
inline HttpResponse HttpResponse::created(bytes body) {
    return {HttpStatusCode::Created, {}, std::move(body)};
}
inline HttpResponse HttpResponse::no_content() {
    return {HttpStatusCode::NoContent, {}, {}};
}
inline HttpResponse HttpResponse::not_found(bytes body) {
    return {HttpStatusCode::NotFound, {}, std::move(body)};
}
inline HttpResponse HttpResponse::error(HttpStatusCode code, std::string message) {
    return {code, {{"Content-Type", "text/plain"}},
            bytes(reinterpret_cast<const uint8_t*>(message.data()), message.size())};
}
```

- [ ] **Step 3: Add TypeTag entries in types.hpp**

In `include/hpactor/types/types.hpp`, add to the `TypeTag` enum:

```cpp
enum class TypeTag : uint32_t {
    Invalid = 0,
    DownMsg = 1,
    ExitMsg = 2,
    LinkMsg = 3,
    UnlinkMsg = 4,
    SpawnRequestTag = 5,
    SpawnResponseTag = 6,
    ErrorMsg = 7,
    HttpRequestTag = 8,    // <-- NEW
    HttpResponseTag = 9,   // <-- NEW
    User = 100,
};
```

- [ ] **Step 4: Verify build**

```bash
ninja -C build
```

Expected: PASS

---

## Task 3: HTTP/1.1 Parser (llhttp Adapter)

**Files:**
- Create: `src/net/http_parser.cpp` (internal, no public header needed beyond llhttp.h)
- Optionally: `include/hpactor/net/http_parser.hpp` if types need to be shared

- [ ] **Step 1: Write HttpParser adapter**

```cpp
// src/net/http_parser.cpp
#include "llhttp.h"
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <functional>

namespace hpactor {
namespace net {

enum class HttpParseState {
    Idle,
    ParsingHeaders,
    ParsingBody,
    Complete,
    Error,
};

class HttpParser {
public:
    using MessageCallback = std::function<void(HttpRequest&&)>;
    using ErrorCallback = std::function<void(llhttp_errno_t, const char*)>;

    HttpParser();
    ~HttpParser();

    // Feed bytes from a read buffer. Returns bytes consumed.
    // On complete message, invokes on_message callback.
    // On error, invokes on_error callback.
    size_t execute(std::span<const uint8_t> data);

    void set_on_message(MessageCallback cb) { on_message_ = std::move(cb); }
    void set_on_error(ErrorCallback cb) { on_error_ = std::move(cb); }

    // Reset parser state for reuse (keep-alive)
    void reset();

    // Accessors
    HttpParseState state() const { return state_; }
    bool upgrade_requested() const { return upgrade_; }
    bool should_keep_alive() const;

private:
    // llhttp callbacks
    static int on_message_begin_cb(llhttp_t* parser);
    static int on_url_cb(llhttp_t* parser, const char* data, size_t len);
    static int on_method_cb(llhttp_t* parser, const char* data, size_t len);
    static int on_header_field_cb(llhttp_t* parser, const char* data, size_t len);
    static int on_header_value_cb(llhttp_t* parser, const char* data, size_t len);
    static int on_headers_complete_cb(llhttp_t* parser);
    static int on_body_cb(llhttp_t* parser, const char* data, size_t len);
    static int on_message_complete_cb(llhttp_t* parser);

    void finish_header();

    llhttp_t parser_;
    llhttp_settings_t settings_;
    HttpParseState state_ = HttpParseState::Idle;
    bool upgrade_ = false;

    // Accumulation buffers
    std::string url_buf_;
    std::string header_name_buf_;
    std::string header_value_buf_;
    std::vector<HttpHeader> headers_;
    HttpMethod method_ = HttpMethod::GET;
    int http_major_ = 1;
    int http_minor_ = 1;
    StreamBuffer body_buf_;

    MessageCallback on_message_;
    ErrorCallback on_error_;
};

// --- Implementation ---

HttpParser::HttpParser() {
    llhttp_settings_init(&settings_);

    settings_.on_message_begin    = on_message_begin_cb;
    settings_.on_url              = on_url_cb;
    settings_.on_method           = on_method_cb;    // llhttp v9.2+
    settings_.on_header_field     = on_header_field_cb;
    settings_.on_header_value     = on_header_value_cb;
    settings_.on_headers_complete = on_headers_complete_cb;
    settings_.on_body             = on_body_cb;
    settings_.on_message_complete = on_message_complete_cb;

    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

HttpParser::~HttpParser() = default;

void HttpParser::reset() {
    llhttp_reset(&parser_);
    url_buf_.clear();
    header_name_buf_.clear();
    header_value_buf_.clear();
    headers_.clear();
    body_buf_.clear();
    method_ = HttpMethod::GET;
    http_major_ = 1;
    http_minor_ = 1;
    upgrade_ = false;
    state_ = HttpParseState::Idle;
}

size_t HttpParser::execute(std::span<const uint8_t> data) {
    if (state_ == HttpParseState::Error) return 0;

    auto result = llhttp_execute(&parser_,
                                  reinterpret_cast<const char*>(data.data()),
                                  data.size());

    if (result == HPE_PAUSED) {
        state_ = HttpParseState::ParsingBody;
    } else if (result != HPE_OK) {
        state_ = HttpParseState::Error;
        if (on_error_) {
            on_error_(result, llhttp_errno_name(result));
        }
        return 0;
    }

    return data.size();
}

bool HttpParser::should_keep_alive() const {
    return llhttp_should_keep_alive(&parser_);
}

// --- llhttp callbacks ---

int HttpParser::on_message_begin_cb(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->state_ = HttpParseState::ParsingHeaders;
    self->url_buf_.clear();
    self->headers_.clear();
    self->body_buf_.clear();
    return 0;
}

int HttpParser::on_url_cb(llhttp_t* parser, const char* data, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->url_buf_.append(data, len);
    return 0;
}

int HttpParser::on_method_cb(llhttp_t* parser, const char* data, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    // Map method string to enum
    std::string method_str(data, len);
    if (method_str == "GET") self->method_ = HttpMethod::GET;
    else if (method_str == "POST") self->method_ = HttpMethod::POST;
    else if (method_str == "PUT") self->method_ = HttpMethod::PUT;
    else if (method_str == "DELETE") self->method_ = HttpMethod::DELETE;
    else if (method_str == "PATCH") self->method_ = HttpMethod::PATCH;
    else if (method_str == "HEAD") self->method_ = HttpMethod::HEAD;
    else if (method_str == "OPTIONS") self->method_ = HttpMethod::OPTIONS;
    return 0;
}

int HttpParser::on_header_field_cb(llhttp_t* parser, const char* data, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    // If we were accumulating a value, finish the previous header
    if (!self->header_value_buf_.empty()) {
        self->finish_header();
    }
    self->header_name_buf_.append(data, len);
    return 0;
}

int HttpParser::on_header_value_cb(llhttp_t* parser, const char* data, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->header_value_buf_.append(data, len);
    return 0;
}

int HttpParser::on_headers_complete_cb(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    // Finish the last header if there was one
    if (!self->header_name_buf_.empty()) {
        self->finish_header();
    }
    self->http_major_ = parser->http_major;
    self->http_minor_ = parser->http_minor;
    self->upgrade_ = parser->upgrade;
    return 0;
}

int HttpParser::on_body_cb(llhttp_t* parser, const char* data, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->body_buf_.append(reinterpret_cast<const uint8_t*>(data), len);
    return 0;
}

int HttpParser::on_message_complete_cb(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->state_ = HttpParseState::Complete;

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

    return 0;
}

void HttpParser::finish_header() {
    headers_.push_back({std::move(header_name_buf_), std::move(header_value_buf_)});
    header_name_buf_.clear();
    header_value_buf_.clear();
}

} // namespace net
} // namespace hpactor
```

- [ ] **Step 2: Verify build and add unit tests**

```bash
ninja -C build
```

Create `tests/net/test_http_parser.cpp` with test cases:

```cpp
// Test cases:
// 1. Parse simple GET request line
// 2. Parse request with headers
// 3. Parse POST with Content-Length body
// 4. Parse chunked transfer encoding
// 5. Parse keep-alive detection
// 6. Parse Connection: close detection
// 7. Parse upgrade request
// 8. Parse pipelined requests (two requests on same connection)
// 9. Parse error: invalid method
// 10. Parse error: invalid HTTP version
// 11. Parser reset and reuse (keep-alive)
```

- [ ] **Step 3: Run tests**

```bash
ctest --output-on-failure -R test_http_parser
```

Expected: 11/11 tests pass

---

## Task 4: HttpSerializer

**Files:**
- Create: `include/hpactor/net/http_serializer.hpp`
- Create: `src/net/http_serializer.cpp`
- Create: `tests/net/test_http_serializer.cpp`

- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/net/http_serializer.hpp
#pragma once

#include <hpactor/types/types.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/actor/typed_message.hpp>

#include <string>
#include <utility>

namespace hpactor {
namespace net {

class HttpSerializer {
public:
    // Ingress: HTTP request body → TypedMessage
    result<TypedMessage>
    deserialize_request(const HttpRequest& req, TypeTag expected_tag);

    // Egress: TypedMessage → HTTP response body + Content-Type
    std::pair<bytes, std::string>
    serialize_response(const TypedMessage& msg,
                       const std::string& accept_header);

    // Egress: TypedMessage → HTTP request body + Content-Type (for HttpClient)
    std::pair<bytes, std::string>
    serialize_request(const TypedMessage& msg);

    // Egress: HTTP response body → TypedMessage
    result<TypedMessage>
    deserialize_response(const HttpResponse& resp, TypeTag expected_tag);

    // Register a JSON ↔ protobuf field mapping for a TypeTag
    // (Phase 1: hand-maintained mapping; Phase 2: protobuf Descriptor reflection)
    struct FieldMapping {
        std::string json_name;
        std::string proto_name;
        enum class Type { Int32, Int64, Uint32, Uint64, Bool, String, Bytes, Float, Double } type;
    };
    void register_mapping(TypeTag tag, std::vector<FieldMapping> fields);

private:
    // JSON parsing helpers
    bytes json_to_protobuf(TypeTag tag, std::span<const uint8_t> json_bytes);
    bytes protobuf_to_json(TypeTag tag, const bytes& proto_bytes);

    // Content-Type parsing
    struct AcceptedType {
        std::string mime_type;
        float quality = 1.0f;
    };
    std::vector<AcceptedType> parse_accept_header(const std::string& header) const;

    std::unordered_map<TypeTag, std::vector<FieldMapping>> mappings_;
};

} // namespace net
} // namespace hpactor
```

- [ ] **Step 2: Implement JSON ↔ protobuf conversion**

For Phase 1, use a hand-maintained mapping table. The serializer uses `DefaultSerializer` for protobuf path and a minimal JSON writer/reader for the JSON path.

JSON writer (protobuf → JSON):
```cpp
bytes HttpSerializer::protobuf_to_json(TypeTag tag, const bytes& proto_bytes) {
    auto it = mappings_.find(tag);
    if (it == mappings_.end()) {
        // No mapping registered — return protobuf as base64-encoded JSON string
        return encode_as_base64_json(proto_bytes);
    }

    std::string json = "{";
    bool first = true;
    for (const auto& field : it->second) {
        if (!first) json += ",";
        first = false;
        json += "\"" + field.json_name + "\":";
        // Extract field value from proto_bytes using field offset/size
        json += extract_json_value(proto_bytes, field);
    }
    json += "}";
    return bytes(json.begin(), json.end());
}
```

JSON reader (JSON → protobuf):
Parse the JSON body, look up the field mapping for the TypeTag, and construct protobuf bytes. For the initial implementation, use a simple recursive-descent JSON parser focused on the subset needed (objects, strings, numbers, booleans, null — no arrays or nested objects initially).

- [ ] **Step 3: Implement content negotiation**

```cpp
HttpSerializer::serialize_response():
1. Parse Accept header into ordered list of AcceptedType
2. For each accepted type (by quality, then specificity):
   a. "application/json" or "*/*" → protobuf_to_json()
   b. "application/x-protobuf" → passthrough proto_bytes
3. Default: "application/json"
```

- [ ] **Step 4: Write tests**

`tests/net/test_http_serializer.cpp`:
```
Test cases:
1. Round-trip: JSON request → TypedMessage → JSON response
2. Round-trip: protobuf request → TypedMessage → protobuf response
3. Content-Type: application/json → deserialize correctly
4. Content-Type: application/x-protobuf → deserialize correctly
5. Content-Type: text/plain → wrap in bytes
6. Content-Type: unsupported → error
7. Accept: application/json → serialize to JSON
8. Accept: application/x-protobuf → serialize to protobuf
9. Accept: */* → default to JSON
10. Accept with quality weights → choose highest quality
11. No Accept header → default to JSON
12. No mapping registered → base64 fallback
```

- [ ] **Step 5: Verify build and run tests**

```bash
ninja -C build && ctest --output-on-failure -R test_http_serializer
```

Expected: 12/12 tests pass

---

## Task 5: Route Registry and Path Pattern Matching

**Files:**
- Extracted into HttpServer header/implementation

- [ ] **Step 1: Implement RouteRegistry**

The route registry is a sorted vector of `(method, pattern, builder)` tuples. Matching is first-registered-first-matched within the same priority.

```cpp
struct RouteEntry {
    HttpMethod method;
    std::vector<PatternSegment> segments; // Pre-parsed pattern
    HttpServer::MessageBuilder builder;
    int priority;
};

enum class PatternSegmentType { Literal, NamedParam, SingleWildcard, MultiWildcard };

struct PatternSegment {
    PatternSegmentType type;
    std::string name; // For Literal: the literal text; for :param: the param name
};

const RouteEntry* RouteRegistry::match(HttpMethod method, const std::string& path) const;
```

- [ ] **Step 2: Implement path pattern matching**

```cpp
// Algorithm:
// 1. Split path by '/'
// 2. For each registered route with matching method:
//    a. Walk pattern segments and path segments in lockstep
//    b. Literal: require exact match
//    c. NamedParam: match any segment, capture value
//    d. SingleWildcard: match any segment, discard value
//    e. MultiWildcard: consume all remaining segments, capture value
//    f. If segment counts don't match (and no MultiWildcard), reject
// 3. Return first successful match (by priority then registration order)
```

- [ ] **Step 3: Verify with tests**

Test cases in the HttpServer test suite:
```
Test cases:
1. Exact literal match
2. Named parameter match + extraction
3. Multiple named parameters
4. Wildcard segment match
5. Trailing wildcard match (multiple segments)
6. No match — wrong method
7. No match — wrong path
8. Priority ordering — higher priority route matches first
9. Method-specific routes (GET vs POST same path)
```

---

## Task 6: HttpServer

**Files:**
- Create: `include/hpactor/net/http_server.hpp`
- Create: `src/net/http_server.cpp`
- Create: `tests/net/test_http_server.cpp`

- [ ] **Step 1: Write the HttpServer header**

```cpp
// include/hpactor/net/http_server.hpp
#pragma once

#include <hpactor/net/http_types.hpp>
#include <hpactor/net/http_serializer.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/core/actor_system.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {
namespace net {

class HttpServer {
public:
    using MessageBuilder = std::function<TypedMessage(const HttpRequest&)>;

    explicit HttpServer(ActorSystem* system);
    ~HttpServer();

    // Lifecycle
    void listen(uint16_t port, std::string host = "0.0.0.0");
    void stop();

    // Route registration (must be called before listen())
    void route(HttpMethod method, std::string path_pattern,
               MessageBuilder builder, int priority = 0);

    // Configuration
    void set_reply_timeout(std::chrono::milliseconds timeout);
    void set_max_connections(size_t max);
    void set_keepalive_timeout(std::chrono::milliseconds timeout);
    void set_max_request_size(size_t max_bytes);

    // Accessors
    uint16_t port() const;
    bool is_running() const;

private:
    void on_accept(int client_fd, CommunicationEndpoint remote);
    void on_read(int client_fd);
    void on_write_complete(int client_fd);
    void on_parse_complete(int client_fd, HttpRequest&& request);
    void on_reply(int client_fd, uint64_t request_id, TypedMessage&& msg);
    void on_timeout(uint64_t request_id);
    void send_error(int client_fd, HttpStatusCode code, const std::string& message);
    void close_connection(int client_fd);

    // Reply collector — receives replies from actors and routes to HTTP connections
    class ReplyCollector : public EventBasedActor {
        // ...
    };

    struct ConnectionCtx {
        int fd;
        std::unique_ptr<HttpParser> parser;
        StreamBuffer read_buf;
        std::vector<bytes> write_queue;
        bool keepalive = true;
        bool write_pending = false;
        uint64_t next_request_id = 1;
    };

    struct PendingReply {
        uint64_t request_id;
        int client_fd;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    ActorSystem* system_;
    EventLoop loop_;
    Acceptor acceptor_;
    std::unique_ptr<HttpSerializer> serializer_;
    RouteRegistry routes_;
    std::unordered_map<int, ConnectionCtx> connections_;
    std::unordered_map<uint64_t, PendingReply> pending_replies_;
    std::chrono::milliseconds reply_timeout_{5000};
    std::chrono::milliseconds keepalive_timeout_{30000};
    size_t max_connections_{1000};
    size_t max_request_size_{1048576};
    uint64_t next_global_request_id_{1};
    bool running_ = false;
};

} // namespace net
} // namespace hpactor
```

- [ ] **Step 2: Implement listen() and accept flow**

```cpp
void HttpServer::listen(uint16_t port, std::string host) {
    acceptor_.listen(port, host);
    loop_.register_read(acceptor_.fd(),
        [this](int fd) {
            auto client = acceptor_.accept(); // Accept one connection
            if (client.fd >= 0 && connections_.size() < max_connections_) {
                auto& ctx = connections_[client.fd];
                ctx.fd = client.fd;
                ctx.parser = std::make_unique<HttpParser>();
                ctx.parser->set_on_message(
                    [this, fd = client.fd](HttpRequest&& req) {
                        on_parse_complete(fd, std::move(req));
                    });
                ctx.parser->set_on_error(
                    [this, fd = client.fd](llhttp_errno_t err, const char* msg) {
                        send_error(fd, HttpStatusCode::BadRequest, msg);
                    });
                loop_.register_read(client.fd,
                    [this](int fd) { on_read(fd); });
            }
        });
    running_ = true;

    // Schedule keepalive timeout check
    loop_.schedule_every(keepalive_timeout_, [this] {
        auto now = std::chrono::steady_clock::now();
        for (auto it = connections_.begin(); it != connections_.end(); ) {
            // Evict idle connections past keepalive_timeout
            // ...
        }
    });
}
```

- [ ] **Step 3: Implement on_read and parse flow**

```cpp
void HttpServer::on_read(int client_fd) {
    auto& ctx = connections_.at(client_fd);

    // Read bytes into StreamBuffer
    uint8_t buf[4096];
    ssize_t n = read(client_fd, buf, sizeof(buf));

    if (n <= 0) {
        close_connection(client_fd);
        return;
    }

    if (ctx.read_buf.size() + n > max_request_size_) {
        send_error(client_fd, HttpStatusCode::PayloadTooLarge, "Request too large");
        return;
    }

    ctx.read_buf.append(buf, static_cast<size_t>(n));

    // Feed to parser
    auto consumed = ctx.parser->execute(
        std::span<const uint8_t>(ctx.read_buf.data(), ctx.read_buf.size()));

    ctx.read_buf.consume(consumed);

    if (ctx.parser->state() == HttpParseState::Error) {
        send_error(client_fd, HttpStatusCode::BadRequest, "Parse error");
    }
}
```

- [ ] **Step 4: Implement request-to-actor delivery**

```cpp
void HttpServer::on_parse_complete(int client_fd, HttpRequest&& request) {
    auto& ctx = connections_.at(client_fd);

    // Match route
    const auto* route = routes_.match(request.method, request.path);
    if (!route) {
        send_error(client_fd, HttpStatusCode::NotFound, "No route for " + request.path);
        return;
    }

    // Build TypedMessage
    TypedMessage msg = route->builder(request);

    // Set reply routing
    uint64_t request_id = next_global_request_id_++;
    ctx.next_request_id = request_id;
    pending_replies_[request_id] = {request_id, client_fd, Clock::now()};

    // Set up reply-to so actor's reply comes back here
    msg.set_sender(reply_collector_->address());
    msg.set_correlation_id(request_id);

    // Deliver to actor
    system_->enqueue(msg.target(), std::move(msg));

    // Schedule timeout
    loop_.schedule_after(reply_timeout_,
        [this, request_id] { on_timeout(request_id); });

    // Track keepalive
    ctx.keepalive = ctx.parser->should_keep_alive();
    ctx.parser->reset();
}
```

- [ ] **Step 5: Implement reply routing and write**

```cpp
void HttpServer::on_reply(int client_fd, uint64_t request_id, TypedMessage&& msg) {
    auto it = pending_replies_.find(request_id);
    if (it == pending_replies_.end()) return; // Already timed out

    auto conn_it = connections_.find(client_fd);
    if (conn_it == connections_.end()) return; // Connection closed

    // Serialize response
    auto [body, content_type] = serializer_->serialize_response(
        msg, /* accept header from original request */);

    // Build HTTP response
    HttpResponse response;
    response.status_code = HttpStatusCode::OK;
    response.headers = {{"Content-Type", content_type},
                        {"Content-Length", std::to_string(body.size())}};
    if (!conn_it->second.keepalive) {
        response.headers.push_back({"Connection", "close"});
    }
    response.body = std::move(body);

    // Serialize to wire format
    bytes wire = serialize_http_response(response);

    // Queue write
    conn_it->second.write_queue.push_back(std::move(wire));
    if (!conn_it->second.write_pending) {
        conn_it->second.write_pending = true;
        auto& queue = conn_it->second.write_queue;
        loop_.async_write(client_fd, queue.front().data(), queue.front().size(),
            [this, fd = client_fd](int result) {
                on_write_complete(fd);
            });
    }

    pending_replies_.erase(it);
}
```

- [ ] **Step 6: Implement keepalive and cleanup**

```cpp
void HttpServer::close_connection(int client_fd) {
    auto it = connections_.find(client_fd);
    if (it == connections_.end()) return;

    loop_.unregister_read(client_fd);
    close(client_fd);

    // Remove pending replies for this connection
    for (auto pit = pending_replies_.begin(); pit != pending_replies_.end(); ) {
        if (pit->second.client_fd == client_fd) {
            pit = pending_replies_.erase(pit);
        } else {
            ++pit;
        }
    }

    connections_.erase(it);
}
```

- [ ] **Step 7: Write integration tests**

`tests/net/test_http_server.cpp`:
```
Test cases:
1. GET request to registered route → 200 with expected body
2. POST request with JSON body → actor receives correctly
3. Actor reply → HTTP response with correct body
4. No matching route → 404
5. Wrong HTTP method for route → 404 (or 405 if we add that)
6. Actor timeout → 504 Gateway Timeout
7. Invalid HTTP → 400 Bad Request
8. Request body too large → 413 Payload Too Large
9. Keep-alive: multiple requests on same connection
10. Connection: close → connection closed after response
11. Concurrent requests (pipeline) → correct response ordering
```

- [ ] **Step 8: Verify build and run tests**

```bash
ninja -C build && ctest --output-on-failure -R test_http_server
```

Expected: 11/11 tests pass

---

## Task 7: HttpClient

**Files:**
- Create: `include/hpactor/net/http_client.hpp`
- Create: `src/net/http_client.cpp`
- Create: `tests/net/test_http_client.cpp`

- [ ] **Step 1: Write the HttpClient header**

```cpp
// include/hpactor/net/http_client.hpp
#pragma once

#include <hpactor/net/http_types.hpp>
#include <hpactor/net/http_serializer.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/rpc/rpc_channel.hpp>  // for RpcFuture

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor {
namespace net {

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
    void abort();

    // Configuration
    void set_default_timeout(std::chrono::milliseconds timeout);
    void set_max_retries(int retries);
    void set_keepalive(bool enable);
    void set_max_connections_per_host(size_t max);

private:
    struct ParsedUrl {
        std::string scheme;
        std::string host;
        uint16_t port;
        std::string path;
    };

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

    ParsedUrl parse_url(const std::string& url);
    void send_request(PendingHttpCall& call);
    void on_response(uint64_t request_id, const HttpResponse& response);
    void on_error(uint64_t request_id, error err);
    void schedule_retry(uint64_t request_id);

    EventLoop* loop_;
    std::unique_ptr<HttpSerializer> serializer_;
    std::unordered_map<std::string, std::shared_ptr<HttpKeepAlivePool>> pools_;
    std::unordered_map<uint64_t, std::unique_ptr<PendingHttpCall>> pending_;
    mutable std::mutex mutex_;
    std::chrono::milliseconds default_timeout_{5000};
    int max_retries_{3};
    bool keepalive_enabled_{true};
    size_t max_connections_per_host_{4};
    uint64_t next_request_id_{1};
};

} // namespace net
} // namespace hpactor
```

- [ ] **Step 2: Implement URL parsing**

```cpp
HttpClient::ParsedUrl HttpClient::parse_url(const std::string& url) {
    ParsedUrl result;

    // Parse scheme
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        result.scheme = "http";
        scheme_end = 0;
    } else {
        result.scheme = url.substr(0, scheme_end);
        scheme_end += 3;
    }

    // Parse host (up to :port or /path)
    auto host_end = url.find_first_of(":/", scheme_end);
    result.host = url.substr(scheme_end, host_end - scheme_end);

    // Parse port
    if (host_end != std::string::npos && url[host_end] == ':') {
        auto port_end = url.find('/', host_end);
        auto port_str = url.substr(host_end + 1,
            port_end == std::string::npos ? std::string::npos : port_end - host_end - 1);
        result.port = static_cast<uint16_t>(std::stoi(port_str));
        host_end = port_end;
    } else {
        result.port = (result.scheme == "https") ? 443 : 80;
    }

    // Parse path
    result.path = (host_end != std::string::npos) ? url.substr(host_end) : "/";

    return result;
}
```

- [ ] **Step 3: Implement request sending**

```cpp
void HttpClient::send_request(PendingHttpCall& call) {
    // Build HTTP request wire format
    std::string request_line = to_string(call.method) + std::string(" ") +
                                call.path + " HTTP/1.1\r\n";

    std::string headers;
    headers += "Host: " + call.host + "\r\n";
    for (const auto& h : call.headers) {
        headers += h.name + ": " + h.value + "\r\n";
    }
    if (call.body.size() > 0) {
        headers += "Content-Length: " + std::to_string(call.body.size()) + "\r\n";
    }
    headers += "Connection: " + std::string(keepalive_enabled_ ? "keep-alive" : "close") + "\r\n";
    headers += "\r\n";

    bytes wire;
    wire.append(reinterpret_cast<const uint8_t*>(request_line.data()), request_line.size());
    wire.append(reinterpret_cast<const uint8_t*>(headers.data()), headers.size());
    wire.append(call.body.data(), call.body.size());

    // Get connection from pool
    auto pool = get_or_create_pool(call.host, call.port);
    auto conn = pool->acquire();
    if (!conn) {
        // Create new connection
        conn = connect_to_host(call.host, call.port);
        if (!conn) {
            on_error(call.request_id, error(errors::http_connect_failed));
            return;
        }
    }

    // Send
    loop_->async_write(conn->fd(), wire.data(), wire.size(),
        [this, request_id = call.request_id](int result) {
            if (result < 0) {
                on_error(request_id, error(errors::http_connect_failed));
            }
        });

    // Register for response
    register_read(conn->fd());
}
```

- [ ] **Step 4: Implement response parsing and retry**

```cpp
void HttpClient::on_response(uint64_t request_id, const HttpResponse& response) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(request_id);
    if (it == pending_.end()) return;

    it->second->promise.set_value(result<HttpResponse>::make(
        HttpResponse{response}));

    pending_.erase(it);
}

void HttpClient::on_error(uint64_t request_id, error err) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(request_id);
    if (it == pending_.end()) return;

    auto& call = *it->second;
    if (call.retry_count < call.max_retries) {
        schedule_retry(request_id);
    } else {
        call.promise.set_value(result<HttpResponse>::make(std::move(err)));
        pending_.erase(it);
    }
}

void HttpClient::schedule_retry(uint64_t request_id) {
    auto& call = *pending_[request_id];
    call.retry_count++;
    auto delay = std::chrono::milliseconds(100 * (1 << call.retry_count)); // 100, 200, 400, 800ms
    if (delay > std::chrono::seconds(10)) delay = std::chrono::seconds(10);

    loop_->schedule_after(delay,
        [this, request_id] {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_.find(request_id);
            if (it != pending_.end()) {
                send_request(*it->second);
            }
        });
}
```

- [ ] **Step 5: Implement keep-alive connection pool**

```cpp
class HttpKeepAlivePool {
public:
    struct PooledConnection {
        int fd;
        std::chrono::steady_clock::time_point last_used;
        bool in_use = false;
    };

    ConnectionHandle acquire();
    void release(int fd, bool keep_alive);
    void evict_idle(std::chrono::milliseconds max_idle);

private:
    std::vector<PooledConnection> connections_;
    size_t max_size_;
};
```

- [ ] **Step 6: Write tests**

`tests/net/test_http_client.cpp`:
```
Test cases:
1. GET request to working HTTP server → 200 with expected body
2. POST request with JSON body → 201 Created
3. Request timeout → error after timeout
4. Connection refused → retry, then error
5. Retry success (first fails, second succeeds)
6. Max retries exceeded → error
7. Keep-alive: connection reused for second request
8. Connection: close → new connection for second request
9. URL parsing: http://host:port/path → correct components
10. URL parsing: https://host/path → default port 443
11. Concurrent requests → responses match correct futures
```

- [ ] **Step 7: Verify build and run tests**

```bash
ninja -C build && ctest --output-on-failure -R test_http_client
```

Expected: 11/11 tests pass

---

## Task 8: ActorContext Integration

**Files:**
- Modify: `include/hpactor/actor_context.hpp`

- [ ] **Step 1: Add HTTP methods to ActorContext**

In `ActorContext`, add:

```cpp
// HTTP egress (delegates to ActorSystem's HttpClient)
RpcFuture<HttpResponse> http_get(const std::string& url,
                                  std::vector<HttpHeader> headers = {});
RpcFuture<HttpResponse> http_post(const std::string& url,
                                   bytes body,
                                   std::vector<HttpHeader> headers = {});
RpcFuture<HttpResponse> http_put(const std::string& url,
                                  bytes body,
                                  std::vector<HttpHeader> headers = {});
RpcFuture<HttpResponse> http_delete(const std::string& url,
                                     std::vector<HttpHeader> headers = {});
RpcFuture<HttpResponse> http_request(HttpMethod method,
                                      const std::string& url,
                                      std::vector<HttpHeader> headers = {},
                                      bytes body = {});
```

Implementation delegates to `ActorSystem::http_client()`:

```cpp
inline RpcFuture<HttpResponse>
ActorContext::http_get(const std::string& url, std::vector<HttpHeader> headers) {
    return system_->http_client()->get(url, std::move(headers));
}
```

- [ ] **Step 2: Verify build**

```bash
ninja -C build
```

---

## Task 9: Final Integration and Documentation

**Files:**
- Modify: `CMakeLists.txt` — add all new test targets
- Modify: `CLAUDE_MEMORY.md` — update project status

- [ ] **Step 1: Add all test targets to CMakeLists.txt**

```cmake
add_executable(test_http_parser tests/net/test_http_parser.cpp)
target_link_libraries(test_http_parser hpactor_lib)
add_test(NAME test_http_parser COMMAND test_http_parser)

add_executable(test_http_server tests/net/test_http_server.cpp)
target_link_libraries(test_http_server hpactor_lib)
add_test(NAME test_http_server COMMAND test_http_server)

add_executable(test_http_client tests/net/test_http_client.cpp)
target_link_libraries(test_http_client hpactor_lib)
add_test(NAME test_http_client COMMAND test_http_client)

add_executable(test_http_serializer tests/net/test_http_serializer.cpp)
target_link_libraries(test_http_serializer hpactor_lib)
add_test(NAME test_http_serializer COMMAND test_http_serializer)
```

- [ ] **Step 2: Full build and test suite**

```bash
cmake -S . -B build -GNinja && ninja -C build && ctest --output-on-failure
```

Expected: All existing 61 tests + ~44 new HTTP tests = ~105 tests passing

- [ ] **Step 3: Update CLAUDE_MEMORY.md**

Add HTTP subsystem to the implemented features and mark Phase 11 progress.

---

## Summary

| Task | Files | Test Count |
|------|-------|-----------|
| 1. Vendor llhttp | `third_party/llhttp/`, CMakeLists.txt | — |
| 2. HTTP Core Types | `http_types.hpp`, `types.hpp` | — |
| 3. HTTP/1.1 Parser | `src/net/http_parser.cpp` | 11 |
| 4. HttpSerializer | `http_serializer.hpp/.cpp` | 12 |
| 5. Route Registry | (in HttpServer header/src) | 9 |
| 6. HttpServer | `http_server.hpp/.cpp` | 11 |
| 7. HttpClient | `http_client.hpp/.cpp` | 11 |
| 8. ActorContext integration | `actor_context.hpp` | — |
| 9. Final integration | CMakeLists.txt, CLAUDE_MEMORY.md | — |

**Total: ~44 new tests, 5 new headers, 4 new source files, 1 vendored dependency**
