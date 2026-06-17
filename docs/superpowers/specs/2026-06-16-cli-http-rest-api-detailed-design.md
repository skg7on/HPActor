# HPActor CLI HTTP REST API — Detailed Design Spec

<!--
Copyright 2026 HPActor Contributors
SPDX-License-Identifier: Apache-2.0
-->

## Metadata

- **Date:** 2026-06-16
- **Status:** Design Approved
- **Architecture Doc:** `docs/architecture/cli/cli-http-rest-api-design.md`
- **Parent PR:** #307 (CLI Architecture Standardization)
- **Affected Subsystem:** `include/hpactor/cli/`, `src/cli/`, `include/hpactor/adt/`

## Table of Contents

1. [Overview](#overview)
2. [Design Decisions Summary](#design-decisions-summary)
3. [Component Architecture](#component-architecture)
4. [ICliCommandHost Integration](#iclicommandhost-integration)
5. [Route Dispatch Flow](#route-dispatch-flow)
6. [Handler Files](#handler-files)
7. [JSON Serialization](#json-serialization)
8. [Error Handling](#error-handling)
9. [Backward Compatibility](#backward-compatibility)
10. [Configuration](#configuration)
11. [Testing Strategy](#testing-strategy)
12. [Files to Create/Modify](#files-to-createmodify)
13. [Build Impact](#build-impact)
14. [Design Review Checklist](#design-review-checklist)

---

## Overview

This spec defines the C++ implementation design for converting the
`CliHttpServerActor` from a single `POST /cli` command-tunnel endpoint to a
full RESTful HTTP API with 24 resource-oriented endpoints.

The architecture-level API contract (URLs, methods, request/response shapes,
status codes, error format) is defined in
`docs/architecture/cli/cli-http-rest-api-design.md`. This spec covers the
**internal C++ implementation** — class structure, routing, serialization,
handler organization, and test design.

### Goals

1. Replace the monolithic `on_http_request()` with a route table using
   `net::HTTPGateway::RouteRegistry`.
2. Implement `ICliCommandHost` on `CliHttpServerActor` so actor operations
   return structured protobufs directly — no `CliSession` intermediary.
3. Per-resource handler files with testable free functions.
4. Declarative `JsonBuilder` class extending `adt/json_helpers` for all
   response serialization.
5. Keep `POST /cli` backward-compatible behind a `legacy_cli_endpoint` flag.
6. All 274 existing CLI tests continue to pass.

---

## Design Decisions Summary

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Actor operations | Implement `ICliCommandHost` directly | Reuses existing host abstraction; no duplicate code paths |
| Handler organization | Per-resource handler files (free functions) | Testable in isolation; manageable file sizes; clear ownership |
| JSON serialization | Declarative `JsonBuilder` in `adt/` | Reusable; reads like the JSON it produces; no external dependency |
| Route matching | `HTTPGateway::RouteRegistry` with `:param` patterns | Already exists; priority-based ordering (literals before params) |
| Error handling | Shared `send_error()`/`send_json()` helpers | Consistent error format across all 24 endpoints |
| Legacy compat | `legacy_cli_endpoint` config flag | Phase 1-2 only; extracts existing code into `cli_http_legacy_handler.cpp` |

---

## Component Architecture

```
HTTP Request (from HTTPGateway)
    │
    ▼
CliHttpServerActor::dispatch_route()
    │  ┌─ RouteEntry: {method, pattern, handler_fn}
    │  │  RouteEntry: {GET,    "/api/v1/actors",            handle_list_actors}
    │  │  RouteEntry: {GET,    "/api/v1/actors/:id",        handle_get_actor}
    │  │  RouteEntry: {DELETE, "/api/v1/actors/:id",        handle_kill_actor}
    │  │  ... (24 routes)
    │  │  RouteEntry: {POST,   "/cli",                       handle_legacy_post_cli}
    │  └─ No match → 404
    │
    ▼
Handler free function (per-resource file)
    │  handle_get_actor(system, host, conn, req)
    │      ├── parse_actor_id(req.path_params["id"])
    │      ├── host->inspect(id, req, timeout)
    │      ├── JsonBuilder::root_object()...
    │      └── send_json(conn, 200, json)
    │
    ▼
ICliCommandHost / ISystemCliHost / ILifecycleCliHost / ActorSystem
    │  CliHttpServerActor implements all three host interfaces
    │  inspect() → try_deliver_local(InspectStateRequest) → wait → InspectStateReply
    │  render_system_stats() → query system_ directly → structured data
    │  drain() / shutdown() → system_.shutdown()
    │
    ▼
HTTP Response (via net::HTTPConnection::send_response)
```

### Layer Dependencies

```
Handlers (free functions) → ICliCommandHost / ISystemCliHost / ILifecycleCliHost
JsonBuilder              → adt/ only (no protobuf, no CLI, no HTTP)
send_error / send_json   → net::HTTPConnection + JsonBuilder
Query param helpers      → net::HttpRequest::query_params
```

No handler depends on `CliSession`, `CommandNode`, `OutputFormatter`, or any
CLI-internal types. This cleanly decouples the HTTP API from the interactive
CLI infrastructure.

---

## ICliCommandHost Integration

### Header Change

`include/hpactor/cli/cli_http_server_actor.hpp`:

```cpp
class CliHttpServerActor : public DaemonActor,
                           public ICliCommandHost,      // NEW
                           public ISystemCliHost,
                           public ILifecycleCliHost {
```

New public methods:

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
```

### Implementation Pattern

The HTTP server is a `DaemonActor` with its own dedicated thread and direct
`ActorSystem&` access. For `inspect`/`kill`/`quarantine`, it uses the
same `try_deliver_local()` + response correlation pattern as `CliLocalActor`:

1. Serialize the request protobuf (`InspectStateRequest`, `KillRequest`,
   `QuarantineRequest`) into a `TypedMessage` payload.
2. Set sender address to the HTTP server actor's own address for reply routing.
3. Call `system_.try_deliver_local(target_actor_id, msg)` to enqueue on the
   target actor's mailbox.
4. The target actor processes the system message and calls
   `context()->reply(reply_msg)`.
5. The HTTP server waits for the reply on a response channel (synchronous,
   with timeout). Return `std::nullopt` on timeout.

For `enumerate()`: iterate the actor registry directly via
`system_.actor_count()` and `system_.get_actor()` — no message passing needed.

### System-Level Host Methods

The `ISystemCliHost` methods (`render_system_stats`, `render_memory_stats`,
`render_fault_status`, `render_dlq_list`) currently take `OutputFormatter&`
and write formatted text. For the REST API, these methods are **not called
by the HTTP handlers**. Instead, the handlers query `ActorSystem` directly:

- `GET /system/stats` → calls `system_.actor_count()`, `system_.scheduler()->worker_count()`, etc.
- `GET /system/memory` → calls `mem::MemoryRegionRegistry::instance().snapshot()`
- `GET /faults` → calls `system_.fault_controller().is_enabled()`, etc.
- `GET /dead-letter-queue` → calls `system_.dead_letter_queue()->snapshot_records()`

The `ISystemCliHost`/`ILifecycleCliHost` methods remain for `POST /cli` legacy
compat and for `CliSession`-based transports. The REST handlers bypass them.

### Response Correlation

The HTTP server needs a mechanism to capture replies from target actors.
Options considered:

**Chosen: Poll the HTTP server actor's own mailbox.** Since `CliHttpServerActor`
is a `DaemonActor` on its own thread, each `ICliCommandHost` method can:

```cpp
std::optional<InspectStateReply> CliHttpServerActor::inspect(
    ActorId target, const InspectStateRequest& req, std::chrono::milliseconds timeout) {
    // 1. Build and send request
    TypedMessage msg(InspectStateRequestTag, serialize_to_bytes(req));
    msg.set_sender_address(address());  // reply comes back to us
    auto enq = system_.try_deliver_local(target, std::move(msg));
    if (!enq.accepted()) return std::nullopt;

    // 2. Poll our own mailbox for the reply (DaemonActor has mailbox access)
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto maybe_msg = mailbox()->try_dequeue();  // non-blocking dequeue
        if (maybe_msg && maybe_msg->type_tag() == InspectStateReplyTag) {
            InspectStateReply reply;
            reply.ParseFromArray(maybe_msg->payload().data(), maybe_msg->payload().size());
            return reply;
        }
        // Yield to avoid busy-spin
        std::this_thread::yield();
    }
    return std::nullopt;  // timeout
}
```

This keeps the implementation simple and self-contained. The existing
`DaemonActor` infrastructure already provides a mailbox. If performance
becomes a concern (unlikely — admin API, not hot path), the polling loop
can be replaced with a condition variable later.

---

## Route Dispatch Flow

### Route Table

A `std::vector<RouteEntry>` sorted by registration order (literal paths
before parameterized). The `RouteEntry` struct:

```cpp
struct RouteEntry {
    net::HttpMethod method;
    std::string pattern;          // e.g., "/api/v1/actors/:id/mailbox"
    RouteHandler handler;         // function pointer
};

using RouteHandler = void (*)(ActorSystem&, CliHttpServerActor*,
                               net::HTTPConnection*, net::HttpRequest&&);
```

Note: `RouteHandler` takes `CliHttpServerActor*` (not `ICliCommandHost*`) so
handlers can access all three host interfaces + `ActorSystem&` via the actor
pointer when needed, but normally they use only the `ICliCommandHost` methods.

### Route Registration

In `CliHttpServerActor::on_daemon_start()`:

```cpp
void CliHttpServerActor::on_daemon_start() {
    // Build route table — ORDER MATTERS: literals before parameterized
    routes_ = {
        // API index
        {GET,  "/api/v1/",                                  handle_api_index},

        // Actors — literal collection path first
        {GET,  "/api/v1/actors",                            handle_list_actors},
        // Actors — parameterized paths
        {GET,  "/api/v1/actors/:id",                        handle_get_actor},
        {DELETE, "/api/v1/actors/:id",                      handle_kill_actor},
        {GET,  "/api/v1/actors/:id/mailbox",                handle_get_mailbox},
        {GET,  "/api/v1/actors/:id/children",               handle_get_children},
        {GET,  "/api/v1/actors/:id/circuit-breaker",        handle_get_circuit_breaker},
        {POST, "/api/v1/actors/:id/circuit-breaker/reset",  handle_reset_circuit_breaker},
        {POST, "/api/v1/actors/:id/quarantine",             handle_quarantine_actor},
        {DELETE, "/api/v1/actors/:id/quarantine",           handle_unquarantine_actor},
        {GET,  "/api/v1/actors/:id/memory",                 handle_get_actor_memory},

        // System
        {GET,  "/api/v1/system",                            handle_get_system},
        {GET,  "/api/v1/system/stats",                      handle_get_system_stats},
        {GET,  "/api/v1/system/memory",                     handle_get_system_memory},
        {POST, "/api/v1/system/drain",                      handle_drain},
        {POST, "/api/v1/system/shutdown",                   handle_shutdown},

        // Faults
        {GET,  "/api/v1/faults",                            handle_get_faults},
        {POST, "/api/v1/faults/clear",                      handle_clear_faults},

        // Dead Letter Queue
        {GET,  "/api/v1/dead-letter-queue",                 handle_list_dlq},
        {GET,  "/api/v1/dead-letter-queue/export",          handle_export_dlq},
        {GET,  "/api/v1/dead-letter-queue/:index",          handle_get_dlq_record},
        {POST, "/api/v1/dead-letter-queue/:index/replay",   handle_replay_dlq},

        // Asks
        {GET,  "/api/v1/asks",                              handle_list_asks},
        {GET,  "/api/v1/asks/:message_id",                  handle_get_ask},
        {POST, "/api/v1/asks/:message_id/cancel",           handle_cancel_ask},
    };

    // Legacy backward compat (Phase 1 only)
    if (config_.legacy_cli_endpoint) {
        routes_.push_back({POST, "/cli", handle_legacy_post_cli});
    }

    // Register single dispatcher with HTTPGateway
    gateway_->set_request_handler(
        [this](net::HTTPConnection* conn, net::HttpRequest&& req) {
            dispatch_route(conn, std::move(req));
        });

    if (!gateway_->listen(config_.http_port, config_.http_bind_address)) {
        listen_ok_ = false;
        return;
    }
    listen_ok_ = true;
}
```

### Path Matching

The `dispatch_route()` method performs linear scan with simple path matching:

```cpp
void CliHttpServerActor::dispatch_route(net::HTTPConnection* conn,
                                         net::HttpRequest&& req) {
    for (const auto& route : routes_) {
        if (route.method != req.method) continue;
        if (match_route_pattern(route.pattern, req.path, req.path_params)) {
            route.handler(system_, this, conn, std::move(req));
            return;
        }
    }
    // No match — 404
    send_error(conn, net::HttpStatusCode::NotFound, "NOT_FOUND",
               std::string(to_string(req.method)) + " " + req.path + " has no handler");
}
```

`match_route_pattern()` splits both the pattern and the path on `/`, then
compares segment-by-segment. `:id`, `:actor_id`, `:index`, `:message_id`
segments in the pattern match any non-empty path segment and capture the
value into `req.path_params`.

**Priority**: Since the route table is a linear vector with literal paths
registered before parameterized ones, `GET /api/v1/actors` matches before
`GET /api/v1/actors/:id`. No priority field needed.

---

## Handler Files

### File Layout

```
src/cli/handlers/
├── cli_http_actor_handlers.cpp      9 actor endpoints
├── cli_http_system_handlers.cpp     5 system endpoints
├── cli_http_dlq_handlers.cpp        4 DLQ endpoints
├── cli_http_fault_handlers.cpp      2 fault endpoints
├── cli_http_ask_handlers.cpp        3 ask endpoints
├── cli_http_legacy_handler.cpp      POST /cli backward compat
└── cli_http_handler_helpers.hpp     shared helpers (parse functions, send_error, send_json)
```

### Handler Function Signature

Every handler has the same signature:

```cpp
void handle_get_actor(ActorSystem& system, CliHttpServerActor* actor,
                      net::HTTPConnection* conn, net::HttpRequest&& req);
```

Where:
- `system` — `ActorSystem&` for registry access, DLQ, scheduler, metrics
- `actor` — `CliHttpServerActor*` for `ICliCommandHost` methods and actor address
- `conn` — `net::HTTPConnection*` for `send_response()`
- `req` — `net::HttpRequest&&` with parsed path, query params, headers, body

### Handler Template

Every handler follows this structure:

```cpp
void handle_get_actor(ActorSystem& system, CliHttpServerActor* actor,
                      net::HTTPConnection* conn, net::HttpRequest&& req) {
    // 1. Extract path params
    auto actor_id = parse_path_uint64(req.path_params, "id");
    if (!actor_id) {
        send_error(conn, HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return;
    }

    // 2. Build request from query params
    InspectStateRequest insp_req;
    apply_inspect_fields(insp_req, parse_fields(req));

    // 3. Perform operation via host interface
    auto reply = actor->inspect(ActorId{*actor_id}, insp_req,
                                std::chrono::milliseconds(2000));
    if (!reply.has_value()) {
        send_error(conn, HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                   "Actor " + std::to_string(*actor_id) + " does not exist");
        return;
    }

    // 4. Serialize and send response
    auto json = build_actor_response(reply.value());
    send_json(conn, HttpStatusCode::OK, json);
}
```

### Handler Inventory

| File | Handlers | Lines (est.) |
|------|----------|--------------|
| `cli_http_actor_handlers.cpp` | `handle_list_actors`, `handle_get_actor`, `handle_kill_actor`, `handle_get_mailbox`, `handle_get_children`, `handle_get_circuit_breaker`, `handle_reset_circuit_breaker`, `handle_quarantine_actor`, `handle_unquarantine_actor`, `handle_get_actor_memory` | ~350 |
| `cli_http_system_handlers.cpp` | `handle_api_index`, `handle_get_system`, `handle_get_system_stats`, `handle_get_system_memory`, `handle_drain`, `handle_shutdown` | ~200 |
| `cli_http_dlq_handlers.cpp` | `handle_list_dlq`, `handle_get_dlq_record`, `handle_replay_dlq`, `handle_export_dlq` | ~150 |
| `cli_http_fault_handlers.cpp` | `handle_get_faults`, `handle_clear_faults` | ~80 |
| `cli_http_ask_handlers.cpp` | `handle_list_asks`, `handle_get_ask`, `handle_cancel_ask` | ~120 |
| `cli_http_legacy_handler.cpp` | `handle_legacy_post_cli` | ~100 |
| `cli_http_handler_helpers.hpp` | `send_error`, `send_json`, `parse_offset`, `parse_limit`, `parse_fields`, `parse_path_uint64`, `match_route_pattern`, helper structs | ~120 |

Total: ~1120 lines of handler code across 7 files.

### JSON Serializer Functions

Each resource type gets a `build_*_response()` function, defined in the
handler file that owns it:

| Function | Defined In |
|----------|-----------|
| `build_actor_response(InspectStateReply&)` | `cli_http_actor_handlers.cpp` |
| `build_actor_list_response(vector<ActorMeta>&, Pagination&)` | `cli_http_actor_handlers.cpp` |
| `build_mailbox_response(MailboxSnapshot&)` | `cli_http_actor_handlers.cpp` |
| `build_children_response(vector<ChildInfo>&)` | `cli_http_actor_handlers.cpp` |
| `build_circuit_breaker_response(CircuitBreakerInfo&)` | `cli_http_actor_handlers.cpp` |
| `build_system_response(...)` | `cli_http_system_handlers.cpp` |
| `build_memory_response(vector<RegionSnapshot>&)` | `cli_http_system_handlers.cpp` |
| `build_fault_response(FaultController&)` | `cli_http_fault_handlers.cpp` |
| `build_dlq_list_response(vector<DeadLetterRecord>&, Pagination&)` | `cli_http_dlq_handlers.cpp` |
| `build_dlq_record_response(DeadLetterRecord&, uint32_t index)` | `cli_http_dlq_handlers.cpp` |
| `build_ask_list_response(...)` | `cli_http_ask_handlers.cpp` |
| `build_ask_detail_response(...)` | `cli_http_ask_handlers.cpp` |

Each uses `JsonBuilder` and follows the JSON shapes defined in the
architecture doc.

---

## JSON Serialization

### JsonBuilder — Declarative API

Located in `include/hpactor/adt/json_helpers.hpp` and `src/adt/json_helpers.cpp`
(alongside existing helpers). The class produces valid JSON via a declarative,
chainable API:

```cpp
class JsonBuilder {
public:
    // Start building — root is an anonymous object
    static JsonBuilder root_object();

    // Keyed nested structures
    JsonBuilder& object(const char* key);         // "key": {
    JsonBuilder& array(const char* key);           // "key": [

    // Anonymous nested structures (for array elements)
    JsonBuilder& object();                         // {
    JsonBuilder& array();                          // [

    // Close current structure — matches innermost { or [
    JsonBuilder& end_object();
    JsonBuilder& end_array();

    // Leaf fields (key: value)
    JsonBuilder& field(const char* key, const std::string& v);
    JsonBuilder& field(const char* key, uint64_t v);
    JsonBuilder& field(const char* key, int64_t v);
    JsonBuilder& field(const char* key, uint32_t v);
    JsonBuilder& field(const char* key, int32_t v);
    JsonBuilder& field(const char* key, double v);
    JsonBuilder& field(const char* key, bool v);
    JsonBuilder& null_field(const char* key);     // "key": null

    // Array elements (no key)
    JsonBuilder& element(const std::string& v);
    JsonBuilder& element(uint64_t v);
    JsonBuilder& element(double v);
    JsonBuilder& element(bool v);

    // Finalize
    std::string build() const;
    void reset();

private:
    struct StackFrame {
        enum Kind { kObject, kArray };
        Kind kind;
        bool needs_comma = false;   // true after first element emitted
    };

    std::string buf_;
    std::vector<StackFrame> stack_;
    bool closed_ = false;

    // Internal: flush pending comma if needed
    void pre_value();
    // Internal: push key with quoting
    void emit_key(const char* key);
    // Internal: push string value with JSON escaping
    void emit_string(const std::string& v);
};
```

### Implementation Notes

**Comma insertion**: `StackFrame::needs_comma` is `false` initially. Before
emitting any value (field, element, or nested structure), `pre_value()`
checks the top frame: if `needs_comma`, append `,`; then set
`needs_comma = true` for next time. This automatically handles
trailing-comma avoidance.

**String escaping**: `emit_string()` delegates to the existing
`adt::json_escape()` function. No new escaping logic.

**Nesting**: `object()`/`array()` push a new `StackFrame` onto `stack_`.
`end_object()`/`end_array()` pop and emit the closing delimiter. Mismatch
(e.g., `end_object()` when top frame is array) is a contract violation and
produces malformed JSON — callers own correctness. No runtime validation
beyond debug assertions.

**Memory**: `buf_` uses `std::string` with occasional reallocation. For
typical admin API responses (1-50KB), this is negligible. The builder
is not designed for streaming gigabytes.

**Thread safety**: Not thread-safe. Each HTTP handler creates a fresh
`JsonBuilder` on the stack. No shared state.

### Usage Example

```cpp
auto json = JsonBuilder::root_object()
    .object("data")
        .field("actor_id", uint64_t(42))
        .field("actor_type", "MetricsActor")
        .field("state", "running")
        .object("mailbox")
            .field("depth", uint32_t(3))
            .field("capacity", uint32_t(1024))
            .field("pressure_state", "low")
            .object("rate_limiter")
                .field("enabled", false)
                .field("rate", 0.0)
                .field("burst", uint32_t(100))
                .field("current_tokens", 50.0)
                .field("blocked_total", uint64_t(0))
            .end_object()
        .end_object()
        .array("children")
            .object()
                .field("actor_id", uint64_t(43))
                .field("actor_type", "HttpConnection")
                .field("state", "running")
            .end_object()
        .end_array()
    .end_object()
    .build();
```

### Error Response

```cpp
void send_error(net::HTTPConnection* conn, net::HttpStatusCode code,
                const std::string& error_code, const std::string& message) {
    auto json = JsonBuilder::root_object()
        .object("error")
            .field("code", error_code)
            .field("message", message)
        .end_object()
        .build();
    send_json(conn, code, json);
}
```

### send_json Helper

```cpp
void send_json(net::HTTPConnection* conn, net::HttpStatusCode code,
               const std::string& json_body) {
    StreamBuffer body(
        reinterpret_cast<const uint8_t*>(json_body.data()),
        json_body.size());
    conn->send_response(code, {{"Content-Type", "application/json"}}, body);
}
```

---

## Error Handling

### Handler Error Flow

Every handler uses the same pattern:

1. **Validate inputs first** — parse path params, check ranges, validate JSON
   body. On failure: `send_error(conn, 400, code, message)` + return.

2. **Perform operation** — call `ICliCommandHost` or `ActorSystem` method.
   On failure: `send_error(conn, 4xx/5xx, code, message)` + return.

3. **Serialize success** — `send_json(conn, 200, json)`.

### Error Code Inventory

| Code | HTTP Status | Trigger |
|------|-------------|---------|
| `INVALID_JSON` | 400 | `parse_json_string_map()` or related parse failure on request body |
| `INVALID_FIELD` | 400 | Path param not a valid uint64, query param value out of range |
| `MISSING_FIELD` | 400 | Required JSON body field absent |
| `INVALID_PAGINATION` | 400 | `offset` or `limit` out of [0, 200] range |
| `FORCE_NOT_SUPPORTED` | 400 | `?force=false` on actor without graceful stop |
| `QUARANTINE_NOT_ENABLED` | 400 | Quarantine on actor without quarantine policy |
| `UNSUPPORTED_MEDIA_TYPE` | 415 | `Content-Type` not `application/json` on POST bodies |
| `ACTOR_NOT_FOUND` | 404 | `inspect()` returned nullopt |
| `CIRCUIT_BREAKER_NOT_CONFIGURED` | 404 | No breaker on target actor |
| `DLQ_INDEX_OUT_OF_RANGE` | 404 | DLQ index exceeds record count |
| `DLQ_NOT_CONFIGURED` | 404 | `system_.dead_letter_queue()` is null |
| `ASK_NOT_FOUND` | 404 | ask message_id not found |
| `ACTOR_NOT_STOPPABLE` | 409 | Kill on system actor (`is_system_actor()` returns true) |
| `REPLAY_DELIVERY_FAILED` | 409 | DLQ `try_pop_at()` + `try_deliver_local()` failed |
| `SYSTEM_NOT_READY` | 503 | `ActorSystem` not fully initialized |
| `SYSTEM_DRAINING` | 503 | System in drain/shutdown phase |
| `INTERNAL_ERROR` | 500 | Unexpected null pointer, logic error |

### Field Selection

`GET /api/v1/actors/:id?fields=metadata,mailbox,children` maps to
`InspectStateRequest` boolean flags:

```cpp
void apply_inspect_fields(InspectStateRequest& req,
                          const std::vector<std::string>& fields) {
    if (fields.empty()) {
        // No field filter — include everything
        req.set_include_state(true);
        req.set_include_mailbox(true);
        req.set_include_children(true);
        req.set_include_circuit_breaker(true);
        req.set_include_quarantine_info(true);
        req.set_include_rate_limiter(true);
        req.set_include_admission(true);
        return;
    }
    for (const auto& f : fields) {
        if (f == "metadata")    { /* always included */ }
        else if (f == "mailbox")      req.set_include_mailbox(true);
        else if (f == "children")     req.set_include_children(true);
        else if (f == "circuit_breaker") req.set_include_circuit_breaker(true);
        else if (f == "quarantine")   req.set_include_quarantine_info(true);
        else if (f == "rate_limiter") req.set_include_rate_limiter(true);
        else if (f == "admission")    req.set_include_admission(true);
    }
}
```

The JSON builder then checks each protobuf `has_*()` method to decide
whether to include that section.

### Query Parameter Helpers

Shared helpers in `cli_http_handler_helpers.hpp`:

```cpp
// Parse a path parameter as uint64. Returns nullopt if missing or non-numeric.
std::optional<uint64_t> parse_path_uint64(
    const std::unordered_map<std::string, std::string>& path_params,
    const std::string& key);

// Parse query parameters
uint32_t parse_offset(const net::HttpRequest& req);  // default 0
uint32_t parse_limit(const net::HttpRequest& req);   // default 50, clamped [1, 200]
std::optional<std::string> parse_query_string(const net::HttpRequest& req,
                                               const std::string& key);
std::optional<uint64_t> parse_query_uint64(const net::HttpRequest& req,
                                            const std::string& key);
std::vector<std::string> parse_fields(const net::HttpRequest& req);
```

---

## Backward Compatibility

### Legacy `POST /cli` Handler

Moved from `CliHttpServerActor::on_http_request()` to
`src/cli/handlers/cli_http_legacy_handler.cpp` as a free function:

```cpp
void handle_legacy_post_cli(ActorSystem& system, CliHttpServerActor* actor,
                            net::HTTPConnection* conn, net::HttpRequest&& req) {
    // Extract body text
    std::string body_str(reinterpret_cast<const char*>(req.body.data()),
                         req.body.size());

    // Parse JSON → CliCommand (unchanged from current code)
    hpactor::cli::CliCommand cmd;
    if (!cli_http_parse_cli_command_json(body_str, cmd)) {
        // ... existing error handling ...
    }

    // Reconstruct command line (unchanged)
    // Route through CliSession (unchanged)
    // Build CliResponse JSON (unchanged)
    // send_json_response (unchanged)
}
```

The legacy handler preserves **100% byte-identical behavior** to the current
implementation. It uses `CliSession` + command tree + `OutputFormatter` as
before. This ensures existing HTTP CLI clients continue to work during the
migration period.

### Config Gate

```cpp
struct CliHttpServerConfig {
    uint16_t http_port = 9090;
    std::string http_bind_address = "127.0.0.1";
    uint32_t max_connections = 100;
    bool legacy_cli_endpoint = true;   // NEW — Phase 1: true, Phase 2: true + warning, Phase 3: false
};
```

### Deprecation Headers (Phase 2)

When `legacy_cli_endpoint` is enabled but deprecated, responses to `POST /cli`
include:
```
Deprecation: true
Sunset: Sat, 01 Jan 2027 00:00:00 GMT
```

This is out of scope for the current implementation but noted here for
completeness.

---

## Configuration

### TOML Schema

```toml
[system.cli.http]
port = 9090
bind_address = "127.0.0.1"
max_connections = 100
legacy_cli_endpoint = true
```

### Config Struct

`include/hpactor/cli/cli_http_server_config.hpp`:

```cpp
struct CliHttpServerConfig {
    uint16_t http_port = 9090;
    std::string http_bind_address = "127.0.0.1";
    uint32_t max_connections = 100;
    bool legacy_cli_endpoint = true;
    std::string default_format = "pretty";  // existing, for legacy CliSession
    uint32_t page_size = 50;                // existing, for legacy CliSession
};
```

Fields `default_format` and `page_size` remain for the legacy `POST /cli`
path (`CliSession` uses them). They have no effect on REST endpoints.

---

## Testing Strategy

### Test File Overview

| File | Tier | Tests (est.) | Focus |
|------|------|--------------|-------|
| `test_cli_http_server.cpp` | Unit | ~40 | Handlers with mock `ICliCommandHost` |
| `test_cli_http_api.cpp` | Integration | ~15 | End-to-end with real `ActorSystem` |
| `test_cli_json_builder.cpp` | Unit | ~15 | `JsonBuilder` edge cases and correctness |

### Unit Tests: `test_cli_http_server.cpp`

**Mock ICliCommandHost** — a test stub that returns canned responses:

```cpp
class MockCommandHost : public ICliCommandHost {
public:
    std::optional<InspectStateReply> inspect_result;
    std::optional<KillReply> kill_result;
    std::optional<QuarantineReply> quarantine_result;
    std::vector<ActorMeta> enumerate_result;

    std::optional<InspectStateReply>
    inspect(ActorId, const InspectStateRequest&, std::chrono::milliseconds) override {
        return inspect_result;
    }
    // ... etc
};
```

**Test cases**:

| # | Test | What's Verified |
|---|------|-----------------|
| 1 | `RouteDispatch_MatchGetActors` | `GET /api/v1/actors` dispatches to `handle_list_actors` |
| 2 | `RouteDispatch_MatchGetActorById` | `GET /api/v1/actors/42` dispatches to `handle_get_actor`, path param captured |
| 3 | `RouteDispatch_NoMatch404` | `GET /api/v1/nonexistent` returns 404 with `NOT_FOUND` error code |
| 4 | `RouteDispatch_WrongMethod405` | `POST /api/v1/actors` returns 404 (no route registered for POST on that path) |
| 5 | `ListActors_Empty` | Empty enumerate → empty data array, total=0 |
| 6 | `ListActors_WithResults` | 3 actors → 3 items in data array, correct metadata fields |
| 7 | `ListActors_PaginationOffset` | `?offset=10&limit=5` → pagination reflects offset |
| 8 | `ListActors_InvalidLimit` | `?limit=0` → clamped to 1; `?limit=500` → clamped to 200 |
| 9 | `GetActor_FullResponse` | All fields → JSON has metadata, mailbox, children, circuit_breaker, quarantine |
| 10 | `GetActor_FieldSelection` | `?fields=metadata,mailbox` → JSON has only those keys |
| 11 | `GetActor_NotFound` | inspect returns nullopt → 404 ACTOR_NOT_FOUND |
| 12 | `GetActor_BadId` | `GET /api/v1/actors/abc` → 400 INVALID_FIELD |
| 13 | `KillActor_Success` | kill returns success → 200, `{"data":{"success":true}}` |
| 14 | `KillActor_SystemActor` | kill returns error → 409 ACTOR_NOT_STOPPABLE |
| 15 | `KillActor_ForceFalseNotSupported` | `?force=false` on unsupported actor → 400 FORCE_NOT_SUPPORTED |
| 16 | `GetMailbox_FullSnapshot` | inspect returns mailbox → all mailbox fields present |
| 17 | `GetChildren_WithChildren` | inspect returns children → array with correct entries |
| 18 | `Quarantine_Success` | quarantine returns success → 200 |
| 19 | `Quarantine_NotEnabled` | quarantine config absent → 400 QUARANTINE_NOT_ENABLED |
| 20 | `Unquarantine_Idempotent` | DELETE quarantine on non-quarantined actor → 200 (idempotent) |
| 21 | `GetSystemStats_Normal` | verify total_actors, running_actors, worker_count in JSON |
| 22 | `GetSystemStats_NotReady` | system not initialized → 503 SYSTEM_NOT_READY |
| 23 | `GetSystemMemory_Regions` | all 6 regions present in regions array |
| 24 | `Drain_Accepted` | POST drain → 202 Accepted |
| 25 | `Shutdown_Accepted` | POST shutdown → 202 Accepted |
| 26 | `GetFaults_Enabled` | fault controller enabled → seed, hooks_triggered present |
| 27 | `GetFaults_Disabled` | fault controller disabled → enabled=false |
| 28 | `ClearFaults_Success` | POST clear → 200 |
| 29 | `ListDLQ_WithRecords` | DLQ has 3 records → 3 items in data array |
| 30 | `ListDLQ_Empty` | DLQ configured but empty → empty array, total=0 |
| 31 | `ListDLQ_NotConfigured` | DLQ is null → empty array, total=0 (not 404 per spec) |
| 32 | `GetDLQRecord_ValidIndex` | record exists → correct fields in response |
| 33 | `GetDLQRecord_OutOfRange` | index too high → 404 DLQ_INDEX_OUT_OF_RANGE |
| 34 | `ReplayDLQ_Success` | replay succeeds → 200 |
| 35 | `ReplayDLQ_DeliveryFailed` | try_deliver_local rejected → 409 REPLAY_DELIVERY_FAILED |
| 36 | `ListAsks_WithPending` | ask manager has pending → 2 items in data |
| 37 | `ListAsks_Empty` | no pending asks → empty array |
| 38 | `CancelAsk_Success` | cancel succeeds → 200 |
| 39 | `LegacyPostCli_ValidCommand` | valid JSON → 200 with CliResponse payload |
| 40 | `LegacyPostCli_InvalidJson` | bad JSON → 400 INVALID_JSON |

### Integration Tests: `test_cli_http_api.cpp`

Uses a real `ActorSystem` with `scheduler_threads=0` for determinism.

| # | Test | Setup |
|---|------|-------|
| 1 | `ActorLifecycleViaHttp` | Spawn actor → GET detail → DELETE kill → GET returns 404 |
| 2 | `ActorInspectionFieldSelection` | `?fields=metadata` → mailbox key absent from response |
| 3 | `QuarantineRoundtrip` | POST quarantine → GET shows quarantined → DELETE unquarantine → GET shows released |
| 4 | `DLQRoundtrip` | Overflow actor's mailbox → GET DLQ → record present → POST replay → message delivered |
| 5 | `SystemDrainShutdown` | GET system/stats → POST drain → POST shutdown |
| 6 | `PaginationConsistency` | Spawn 100 actors → iterate ?offset=0,10,20,... → verify total count correct |
| 7 | `FaultStatusAfterClear` | GET faults (hooks_triggered > 0) → POST clear → GET faults (hooks_triggered = 0) |
| 8 | `LegacyPostCli_BackwardCompat` | POST /cli with existing JSON format → response matches current behavior |
| 9 | `ApiIndex` | GET /api/v1/ → response contains endpoint map with correct URLs |
| 10 | `MethodNotAllowed` | POST to GET-only endpoint → 404 (no matching route) |

### JsonBuilder Unit Tests: `test_cli_json_builder.cpp`

| # | Test |
|---|------|
| 1 | `RootObject_Empty` → `{}` |
| 2 | `SingleStringField` → `{"key":"value"}` |
| 3 | `AllNumericTypes` → uint64, int64, uint32, double |
| 4 | `BooleanAndNull` → true, false, null |
| 5 | `NestedObject` → `{"outer":{"inner":"value"}}` |
| 6 | `SimpleArray` → `{"items":[1,2,3]}` |
| 7 | `ArrayOfObjects` → `{"children":[{"id":1},{"id":2}]}` |
| 8 | `StringEscaping` → quotes, backslashes, newlines, tabs |
| 9 | `DeepNesting` → 10 levels of objects, verifies closing |
| 10 | `ResetAndReuse` → build, reset, build again with different content |
| 11 | `EmptyArray` → `{"items":[]}` |
| 12 | `EmptyObjectInArray` → `{"items":[{},{}]}` |
| 13 | `CommaPlacement` → multiple fields, no trailing comma |
| 14 | `RootObjectShortcut` → `JsonBuilder::root_object().field("a", uint64_t(1)).end_object().build()` |
| 15 | `ErrorEnvelope` → canonical error response format |

---

## Files to Create/Modify

| Action | File | Lines (est.) | Description |
|--------|------|--------------|-------------|
| **MODIFY** | `include/hpactor/cli/cli_http_server_actor.hpp` | +30 | Add `ICliCommandHost`, new method declarations, route table members |
| **MODIFY** | `src/cli/cli_http_server_actor.cpp` | +100, −350 | Replace `on_http_request()` with `dispatch_route()`, add `ICliCommandHost` impl, route registration |
| **MODIFY** | `include/hpactor/cli/cli_http_server_config.hpp` | +3 | Add `legacy_cli_endpoint` field |
| **MODIFY** | `include/hpactor/adt/json_helpers.hpp` | +60 | Add `JsonBuilder` class declaration |
| **MODIFY** | `src/adt/json_helpers.cpp` | +120 | Add `JsonBuilder` implementation |
| **ADD** | `src/cli/handlers/cli_http_handler_helpers.hpp` | +120 | Shared helpers: `send_error`, `send_json`, query param parsers, route matching |
| **ADD** | `src/cli/handlers/cli_http_actor_handlers.cpp` | +350 | 9 actor endpoint handlers + `build_*_response()` functions |
| **ADD** | `src/cli/handlers/cli_http_system_handlers.cpp` | +200 | 5 system endpoint handlers + `build_*_response()` functions |
| **ADD** | `src/cli/handlers/cli_http_dlq_handlers.cpp` | +150 | 4 DLQ endpoint handlers + `build_*_response()` functions |
| **ADD** | `src/cli/handlers/cli_http_fault_handlers.cpp` | +80 | 2 fault endpoint handlers + `build_*_response()` functions |
| **ADD** | `src/cli/handlers/cli_http_ask_handlers.cpp` | +120 | 3 ask endpoint handlers + `build_*_response()` functions |
| **ADD** | `src/cli/handlers/cli_http_legacy_handler.cpp` | +100 | Extracted `POST /cli` handler (unchanged logic) |
| **MODIFY** | `src/CMakeLists.txt` | +7 | Add new handler source files |
| **MODIFY** | `tests/unit/cli/test_cli_http_server.cpp` | +350 | Rewrite for ~40 REST endpoint unit tests |
| **MODIFY** | `tests/unit/cli/CMakeLists.txt` | +1 | Add `test_cli_json_builder` target |
| **ADD** | `tests/unit/cli/test_cli_json_builder.cpp` | +120 | ~15 JsonBuilder unit tests |
| **ADD** | `tests/integration/cli/test_cli_http_api.cpp` | +250 | ~10 end-to-end integration tests |
| **MODIFY** | `tests/integration/cli/CMakeLists.txt` | +1 | Add `test_cli_http_api` target |

**Summary**: 8 new files, 7 modified files. ~1120 lines of handler code,
~720 lines of test code. Net change: approximately +1200 lines after
removing the ~350-line `on_http_request()`.

---

## Build Impact

### CMakeLists.txt Changes

```cmake
# src/CMakeLists.txt — add handler source files
set(CLI_HTTP_HANDLER_SOURCES
    src/cli/handlers/cli_http_actor_handlers.cpp
    src/cli/handlers/cli_http_system_handlers.cpp
    src/cli/handlers/cli_http_dlq_handlers.cpp
    src/cli/handlers/cli_http_fault_handlers.cpp
    src/cli/handlers/cli_http_ask_handlers.cpp
    src/cli/handlers/cli_http_legacy_handler.cpp
)
target_sources(hpactor_lib PRIVATE ${CLI_HTTP_HANDLER_SOURCES})
```

### Compile-Time Impact

The handler files include:
- `<hpactor/cli/cli_http_server_actor.hpp>` (for `CliHttpServerActor*` cast)
- `<hpactor/cli/cli_command_host.hpp>` (for `ICliCommandHost` interface)
- `<hpactor/adt/json_helpers.hpp>` (for `JsonBuilder`)
- `<hpactor/net/http_types.hpp>` (for `HttpStatusCode`, `HttpRequest`)
- `<hpactor/net/http_connection.hpp>` (for `send_response()`)

No new external dependencies. The handler files compile as part of
`hpactor_lib` (alongside existing CLI sources). Incremental build impact
is proportional to the number of handler files changed (~6 new .cpp files,
1 modified existing file).

### Test Binary Impact

- `test_cli_http_server` — existing binary, targets added
- `test_cli_json_builder` — new binary, links `hpactor_lib`
- `test_cli_http_api` — new binary, links `hpactor_lib`

---

## Design Review Checklist

- [x] No exceptions, no RTTI — `JsonBuilder` uses return values, not throw
- [x] No `dynamic_cast` or `typeid`
- [x] No shared mutable state between `JsonBuilder` instances (stack-local)
- [x] Bounded capacity — `JsonBuilder::buf_` grows with response size but admin API responses are bounded
- [x] Blocking I/O on daemon thread — HTTP server has its own thread (DaemonActor), blocking in `ICliCommandHost` methods is acceptable
- [x] Existing tests preserved — 274 CLI tests unaffected (new handlers in new files, legacy handler extracted unchanged)
- [x] Backward compatible — `POST /cli` preserved behind `legacy_cli_endpoint` flag
- [x] Subsystem-owned config — `legacy_cli_endpoint` in `CliHttpServerConfig`, not a central switch
- [x] Deterministic tests — mock `ICliCommandHost` for unit tests, `scheduler_threads=0` for integration
- [x] Architecture doc aligned — all endpoint shapes match `docs/architecture/cli/cli-http-rest-api-design.md`

---

## References

- Architecture Doc: `docs/architecture/cli/cli-http-rest-api-design.md`
- PR #307: CLI Architecture Standardization
- `include/hpactor/cli/cli_command_host.hpp` — host interfaces
- `include/hpactor/cli/cli_http_server_actor.hpp` — current implementation
- `include/hpactor/adt/json_helpers.hpp` — existing JSON helpers
- `include/hpactor/net/http_types.hpp` — HTTP types (HttpMethod, HttpStatusCode, HttpRequest)
- `include/hpactor/net/http_gateway.hpp` — HTTPGateway + RouteRegistry
- `docs/architecture/cli/cli-architecture-detailed-design.md` — CLI architecture
