# HTTP Communication

The HTTP subsystem enables actors to communicate through HTTP at system boundaries — external clients calling actors via REST APIs, actors calling external web services, and (as a natural consequence) actor-to-actor communication over HTTP when the network demands it.

## The Big Picture

The existing protobuf-over-TCP transport is efficient for actor-to-actor communication within a trusted network. HTTP addresses the boundaries:

- **Ingress (External → Actors)**: REST APIs, webhooks, browser clients, mobile apps
- **Egress (Actors → External)**: Third-party APIs, OAuth token exchange, webhook delivery
- **Cross-boundary (Actor ↔ Actor)**: Firewall traversal, load balancer integration, CDN caching

The core design principle: **actors remain transport-agnostic**. An actor handles `TypedMessage` and calls `context()->reply()` — it never knows whether the caller arrived via TCP, HTTP, or a local mailbox. The HTTP subsystem is a translation layer at the boundary.

```
                        ┌──────────────────────────────────────┐
                        │           ActorSystem                 │
                        │                                       │
  HTTP Client ─────────►│  ┌──────────────┐                     │
  (REST, browser,       │  │  HttpServer  │                     │
   webhook)             │  │  (ingress)   │                     │
                        │  └──┬───────────┘                     │
                        │     │ TypedMessage                    │
                        │     ▼                                 │
                        │  ┌──────────────┐                     │
                        │  │    Actors    │                     │
                        │  │ (unchanged)  │                     │
                        │  └──┬───────────┘                     │
                        │     │ TypedMessage                    │
                        │     ▼                                 │
  External API ◄────────│  ┌──────────────┐                     │
  (third-party HTTP)    │  │  HttpClient  │                     │
                        │  │  (egress)    │                     │
                        │  └──────────────┘                     │
                        └──────────────────────────────────────┘
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

Three new components form the HTTP subsystem:

```
┌─────────────────────────────────────────────────────────────┐
│                    HTTP Subsystem                            │
│                                                              │
│  ┌──────────────────┐    ┌──────────────────┐               │
│  │    HttpServer    │    │    HttpClient    │               │
│  │                  │    │                  │               │
│  │ Route registry   │    │ Connection pool  │               │
│  │ Request parsing  │    │ Request builder  │               │
│  │ Reply routing    │    │ Response parser  │               │
│  │ Timeout/errors   │    │ Timeout/retry    │               │
│  └────────┬─────────┘    └────────┬─────────┘               │
│           │                       │                          │
│           └───────────┬───────────┘                          │
│                       │                                      │
│              ┌────────▼────────┐                             │
│              │ HttpSerializer  │                             │
│              │                 │                             │
│              │ JSON ↔ protobuf │                             │
│              │ Content-Type    │                             │
│              │ negotiation     │                             │
│              └────────┬────────┘                             │
│                       │                                      │
│              ┌────────▼────────┐                             │
│              │  HTTP/1.1       │                             │
│              │  Parser (llhttp)│                             │
│              └─────────────────┘                             │
└─────────────────────────────────────────────────────────────┘
```

### HttpServer (Ingress)

A gateway that listens on an HTTP port and translates incoming HTTP requests into actor messages. It is structurally a sibling of `Acceptor` — an EventLoop-integrated component managed by ActorSystem, not an actor itself.

**Core responsibilities:**
- Accept HTTP connections via EventLoop async I/O
- Parse HTTP/1.1 requests using llhttp
- Match URL path + method against a route registry
- Convert HTTP requests to `TypedMessage` via user-registered builder functions
- Deliver messages to actors and collect replies via `context()->reply()`
- Convert reply `TypedMessage` back to HTTP responses via HttpSerializer
- Handle timeouts (504), missing routes (404), and serialization errors (400)

**Route registry**: Routes map `(HttpMethod, path_pattern)` → `(target actor, message builder)`. Path patterns support named parameters (`:id`), wildcard segments (`*`), and trailing wildcards (`*path`). Routes are registered programmatically before `listen()` is called; they are immutable after listen.

### HttpClient (Egress)

Enables actors to call external HTTP services. Follows the `RpcChannel` pattern — async request with a `RpcFuture<HttpResponse>` for the response.

**Core responsibilities:**
- Build and send HTTP requests to external URLs
- Parse HTTP responses via llhttp
- Maintain a keep-alive connection pool per destination host
- Retry with exponential backoff on transient failures
- Expose timeout-enforced `RpcFuture<HttpResponse>` for result retrieval
- Integrate into `ActorContext` for actor-thread callers

### HttpSerializer (Content Negotiation)

Bridges between HTTP wire formats and protobuf TypedMessages.

| Format | Content-Type | Ingress | Egress |
|--------|-------------|---------|--------|
| JSON | `application/json` | JSON → protobuf | protobuf → JSON |
| Protobuf binary | `application/x-protobuf` | Passthrough | Passthrough |
| Plain text | `text/plain` | Wrap in `bytes` payload | `bytes` → raw text |

### HTTP/1.1 Parser

The project embeds [llhttp](https://github.com/nodejs/llhttp) (MIT license, single `.c`/`.h` pair, ~4K lines), the same C library that powers Node.js's HTTP parser. It never allocates, never blocks, and integrates with the existing `StreamBuffer` for incremental data consumption.

## End-to-End Flow: Ingress

1. EventLoop delivers readable event on the HTTP listen socket
2. HttpServer accepts the connection, creates per-connection llhttp parser + StreamBuffer
3. llhttp parses the request line, headers, and body through callbacks
4. On `on_message_complete`, the parsed request is matched against the route registry
5. The matched route's builder function produces a `TypedMessage` with `sender` set to the HttpServer's reply-collector address
6. Message is delivered to the target actor
7. HttpServer stores a pending response entry: `(connection_fd, request_id) → promise`
8. The actor processes the message and calls `context()->reply(response_msg)`
9. The reply routes to the HttpServer's reply collector
10. HttpServer looks up the pending response, converts `TypedMessage` → `HttpResponse` via HttpSerializer
11. HTTP response is formatted and written to the connection
12. If the actor doesn't reply within the timeout, HttpServer sends `504 Gateway Timeout`

## Error Mapping

| Actor/system error | HTTP status |
|-------------------|-------------|
| Actor not found | 404 Not Found |
| Mailbox full | 503 Service Unavailable |
| Timeout (no reply) | 504 Gateway Timeout |
| Serialization failure | 400 Bad Request |
| No matching route | 404 Not Found |
| Unsupported Content-Type | 415 Unsupported Media Type |
| Internal error | 500 Internal Server Error |

## Relationship to Existing Components

| Existing Component | HTTP Equivalent | Relationship |
|-------------------|----------------|-------------|
| `Acceptor` | HttpServer listener | Same EventLoop async-accept pattern |
| `RpcChannel` | HttpClient | Same pending-call map + timeout + retry |
| `DefaultSerializer` | HttpSerializer | Same pluggable encode/decode, adds JSON |
| `ConnectionPool` | HTTP keep-alive pool | Same round-robin + exponential backoff |
| `EventLoop` | EventLoop (reused) | Both components use the existing EventLoop |

## Design Constraints

- **No new external dependencies** except vendored llhttp (MIT, single `.c`/`.h`)
- **Actors are unchanged** — no HTTP-specific actor base classes, no HTTP awareness in actor code
- **`-fno-exceptions`, `-fno-rtti`** — error handling via `error` type and result types, not exceptions
- **HTTP/1.1 only** for the initial implementation; HTTP/2 and WebSocket are future extensions
- **JSON is the default wire format** for HTTP; protobuf binary is an opt-in optimization
- **HttpServer is not an actor** — it is an EventLoop component like `Acceptor`, owned by ActorSystem

## Files

| File | Purpose |
|------|---------|
| `include/hpactor/net/http_types.hpp` | HttpMethod, HttpStatusCode, HttpHeader, HttpRequest, HttpResponse |
| `include/hpactor/net/http_server.hpp` | HttpServer class declaration |
| `include/hpactor/net/http_client.hpp` | HttpClient class declaration |
| `include/hpactor/net/http_serializer.hpp` | HttpSerializer — JSON ↔ protobuf with content negotiation |
| `src/net/http_server.cpp` | HttpServer implementation |
| `src/net/http_client.cpp` | HttpClient implementation |
| `src/net/http_parser.cpp` | llhttp StreamBuffer adapter |
| `src/net/http_serializer.cpp` | HttpSerializer implementation |
| `third_party/llhttp/` | Vendored llhttp source |
