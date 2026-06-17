# HPActor CLI — RESTful HTTP API Design

<!--
Copyright 2026 HPActor Contributors
SPDX-License-Identifier: Apache-2.0
-->

## Table of Contents

1. [Overview](#overview)
2. [Design Principles](#design-principles)
3. [Current State & Problems](#current-state--problems)
4. [Resource Model](#resource-model)
5. [API Conventions](#api-conventions)
6. [Endpoint Reference](#endpoint-reference)
7. [Data Types](#data-types)
8. [Error Codes](#error-codes)
9. [Client Implementation Guide](#client-implementation-guide)
10. [Migration from `POST /cli`](#migration-from-post-cli)
11. [Implementation Plan](#implementation-plan)

---

## Overview

This document defines the RESTful HTTP API for the HPActor actor system CLI. It
replaces the current `POST /cli` command-tunnel endpoint with a resource-oriented
API that follows standard REST conventions.

### Goals

- **Resource-oriented URLs** — each entity (actor, DLQ record, fault state) has
  a stable, predictable URL.
- **Semantic HTTP methods** — GET reads, POST creates/acts, DELETE removes.
- **Structured JSON** — all responses are `application/json` with consistent
  envelope shapes.
- **Proper HTTP status codes** — clients can use standard HTTP error handling.
- **Discoverable** — navigating from `/api/v1/` reveals available resources.
- **Decoupled from CLI internals** — no command-tree, tokenizer, or
  `OutputFormatter` involvement in the HTTP path.

### Non-Goals

- Replacing the protobuf wire protocol (`CliCommand`/`CliResponse`) used by
  `CliClientActor` ↔ `CliProtoServerActor`. The protobuf binary protocol
  remains the high-performance path for CLI-to-daemon communication.
- Changing the interactive `CliActor`/`CliLocalActor` or `CliLegacyServerActor`
  (text-based TCP/UDS CLI). Those remain unchanged.
- Health endpoints (`/health/live`, `/health/ready`) — these are infrastructure
  probes served by `HealthHttpServer` on port 8089 and are out of scope.

### Audience

This spec is written for developers implementing an HTTP-based CLI client in any
language (Go, Python, Rust, TypeScript, etc.). No knowledge of HPActor C++
internals is required — just standard HTTP + JSON.

---

## Design Principles

### 1. Resources, Not RPC

Every URL names a **resource** (noun), not an action (verb). HTTP methods
provide the verb:

| Method | Semantics |
|--------|-----------|
| `GET` | Read a resource or collection. Safe, idempotent. |
| `POST` | Create a resource or trigger an action. Not idempotent. |
| `DELETE` | Remove a resource. Idempotent. |

Actions that don't map cleanly to CRUD use `POST` on a verb-named sub-resource
(e.g., `POST /actors/{id}/quarantine`, `POST /dead-letter-queue/{index}/replay`).

### 2. Consistent JSON Envelope

**Single-item success response:**
```json
{
  "data": { ... }
}
```

**Collection success response:**
```json
{
  "data": [ ... ],
  "pagination": {
    "offset": 0,
    "limit": 50,
    "total": 142
  }
}
```

**Error response:**
```json
{
  "error": {
    "code": "ACTOR_NOT_FOUND",
    "message": "Actor with id 42 does not exist"
  }
}
```

**Action result response:**
```json
{
  "data": {
    "success": true
  }
}
```

### 3. HTTP Status Codes

| Code | When |
|------|------|
| `200 OK` | Successful GET, successful action POST |
| `201 Created` | Resource created (future use) |
| `202 Accepted` | Async operation started (drain, shutdown) |
| `204 No Content` | Successful DELETE with no body |
| `400 Bad Request` | Malformed JSON, missing required fields, invalid values |
| `404 Not Found` | Resource does not exist |
| `405 Method Not Allowed` | Wrong HTTP method for the endpoint |
| `409 Conflict` | Operation conflicts with current state (e.g., kill already-terminated actor) |
| `500 Internal Server Error` | Unexpected server-side failure |
| `503 Service Unavailable` | Actor system not ready (still starting, draining, or shutting down) |

### 4. API Versioning

All endpoints are prefixed with `/api/v1/`. This allows future API versions to
coexist during migration periods. The version is part of the URL (not a header)
for visibility and simplicity.

### 5. Content Negotiation

- All request bodies must be `Content-Type: application/json`.
- All response bodies are `Content-Type: application/json`.
- The `Accept` header is respected; only `application/json` (and
  `application/json; charset=utf-8`) is supported. Requests for other media
  types receive `406 Not Acceptable`.
- Output format selection (`pretty`, `json`, `tabular`) is a CLI concept and
  has no equivalent in the REST API. All HTTP responses are JSON.

### 6. Filtering & Field Selection

- **Pagination**: `?offset=<n>&limit=<n>` (offset-based, default limit=50, max=200).
- **Filtering**: `?actor_type=<string>`, `?state=<string>` on collection endpoints.
- **Field selection**: `?fields=metadata,mailbox,children` on detail endpoints.
  When omitted, all fields are returned. When specified, only matching top-level
  keys appear in the response.

### 7. Naming Conventions

- URL paths: lowercase, hyphen-separated (`/dead-letter-queue`, `/circuit-breaker`).
- JSON keys: `snake_case` (matches the protobuf JSON convention).
- Query parameters: `snake_case` (`?actor_type=`, `?actor_id=`).
- Error codes: `UPPER_SNAKE_CASE` (`ACTOR_NOT_FOUND`, `MAILBOX_FULL`).

---

## Current State & Problems

The current `CliHttpServerActor` (`src/cli/cli_http_server_actor.cpp`) has a
single endpoint:

```
POST /cli
Content-Type: application/json

{
  "path": "actor/42/show",
  "params": {"json": "true"},
  "format": "json"
}
```

Problems with this approach:

1. **RPC tunnel** — one URL for all operations. No resource model.
2. **Double parsing** — JSON → `CliCommand` protobuf → reconstructed CLI string
   → `CliSession` tokenization → command-tree dispatch. Wasteful.
3. **Opaque errors** — failures return `200 OK` with `"is_error": true` in
   the body. HTTP intermediaries can't observe failures.
4. **CLI leak** — `path`, `params`, `format` are CLI concepts. HTTP has URLs,
   query params, and `Accept` headers for these.
5. **Text-blob responses** — even when the underlying data is structured
   (`InspectStateReply`, `SystemStatsReply`), the response is a formatted text
   string wrapped in `CliResponse.payload`.
6. **No `ICliCommandHost`** — the HTTP server doesn't implement the
   `ICliCommandHost` interface, so it can't do structured actor operations
   directly. Everything goes through the command tree.

---

## Resource Model

```
/api/v1/
├── actors                           Actor collection
│   └── {actor_id}                   Individual actor
│       ├── mailbox                  Actor's mailbox snapshot
│       ├── children                 Actor's children
│       ├── circuit-breaker          Circuit breaker state
│       ├── quarantine               Quarantine state (POST to set, DELETE to clear)
│       └── memory                   Per-actor memory statistics
├── system                           System overview
│   ├── stats                        System statistics
│   ├── memory                       System-wide memory statistics
│   ├── drain                        POST to initiate drain
│   └── shutdown                     POST to initiate shutdown
├── faults                           Fault injection state
│   └── clear                        POST to clear fault counters
├── dead-letter-queue                DLQ records
│   ├── {index}                      Individual DLQ record
│   │   └── replay                   POST to replay this record
│   └── export                       GET to export all records
├── asks                             Pending ask requests
│   └── {message_id}                 Individual ask
│       └── cancel                   POST to cancel this ask
└── endpoints                        Network endpoints (future)
```

---

## API Conventions

### Authentication

Version 1 of the API does not require authentication. The HTTP server binds to
`127.0.0.1` by default. Future versions will add mTLS or bearer-token auth for
remote access.

### Idempotency

- `GET`, `DELETE` are idempotent.
- `POST` is not idempotent by default. Clients should not retry `POST` requests
  without understanding the operation semantics.
- `POST /actors/{id}/kill` (force kill) and `DELETE /actors/{id}` are idempotent
  — killing an already-terminated actor returns `200 OK` (not `404`).

### Rate Limiting

Version 1 does not enforce rate limits at the HTTP layer. The underlying actor
system's mailbox admission and backpressure mechanisms provide natural
throttling. Future versions may add `429 Too Many Requests` with `Retry-After`.

### Timestamps

All timestamps are returned as Unix milliseconds (int64) in JSON. This is
monotonic (not wall-clock) for internal durations, and system-clock for
absolute times.

---

## Endpoint Reference

### 1. Actors

#### `GET /api/v1/actors`

List actors in the system.

**Query Parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `offset` | integer | 0 | Pagination offset |
| `limit` | integer | 50 | Max results (1–200) |
| `actor_type` | string | — | Filter by actor type substring |
| `state` | string | — | Filter by lifecycle state (`running`, `idle`, `stopping`, etc.) |

**Response `200 OK`:**
```json
{
  "data": [
    {
      "actor_id": 42,
      "actor_type": "MetricsActor",
      "state": "running",
      "incarnation": 1,
      "messages_processed": 15003,
      "uptime_ms": 3600000,
      "behavior_name": "metrics_behavior"
    }
  ],
  "pagination": {
    "offset": 0,
    "limit": 50,
    "total": 142
  }
}
```

**Notes:**
- The `total` count reflects the number of actors matching the filter, not the
  total number in the system (unless no filter is provided).
- Pagination is offset-based, not cursor-based. This is acceptable for an admin
  API where the actor count is expected to be O(1000–10000), not millions.

---

#### `GET /api/v1/actors/{actor_id}`

Get detailed information about a specific actor.

**Query Parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `fields` | string | — | Comma-separated list of fields to include. Omit for all fields. Options: `metadata`, `mailbox`, `children`, `circuit_breaker`, `quarantine`, `rate_limiter`, `admission` |

**Response `200 OK` (all fields):**
```json
{
  "data": {
    "metadata": {
      "actor_id": 42,
      "actor_type": "MetricsActor",
      "state": "running",
      "incarnation": 1,
      "messages_processed": 15003,
      "uptime_ms": 3600000,
      "behavior_name": "metrics_behavior"
    },
    "mailbox": {
      "depth": 3,
      "total_enqueued": 15003,
      "total_dequeued": 15000,
      "max_depth": 128,
      "high_priority_depth": 0,
      "capacity": 1024,
      "queued_bytes": 384,
      "byte_capacity": 65536,
      "pressure_ratio_ppm": 2929,
      "total_rejected": 0,
      "total_dropped": 0,
      "total_dead_letters": 0,
      "pressure_state": "low",
      "overflow_policy": "dead_letter_queue",
      "rate_limiter": {
        "enabled": false,
        "rate": 0.0,
        "burst": 0,
        "current_tokens": 0.0,
        "blocked_total": 0
      },
      "admission": {
        "policy_count": 1,
        "rejected_total": 0,
        "dlq_routed_total": 0
      },
      "delivery": {
        "accepted_total": 15000,
        "rejected_total": 0,
        "failed_total": 0,
        "retryable_total": 0
      }
    },
    "children": [
      {
        "actor_id": 43,
        "actor_type": "HttpConnection",
        "state": "running"
      }
    ],
    "circuit_breaker": {
      "state": "closed",
      "trip_count": 0,
      "failure_ema": 0.0,
      "opened_at_ms": 0
    },
    "quarantine": {
      "enabled": true,
      "quarantined": false,
      "reason": ""
    }
  }
}
```

**Response `200 OK` (selected fields `?fields=metadata,mailbox`):**
```json
{
  "data": {
    "metadata": { ... },
    "mailbox": { ... }
  }
}
```

**Response `404 Not Found`:**
```json
{
  "error": {
    "code": "ACTOR_NOT_FOUND",
    "message": "Actor with id 42 does not exist"
  }
}
```

---

#### `DELETE /api/v1/actors/{actor_id}`

Kill (force-stop) an actor. This is the preferred way to terminate an actor via
the REST API.

**Query Parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `force` | boolean | true | If `false`, graceful stop is requested. If `true`, immediate termination. |

**Response `200 OK`:**
```json
{
  "data": {
    "success": true
  }
}
```

**Response `409 Conflict` (graceful stop rejected):**
```json
{
  "error": {
    "code": "ACTOR_NOT_STOPPABLE",
    "message": "Actor 42 is a system actor and cannot be stopped"
  }
}
```

**Notes:**
- Killing an already-terminated actor returns `200 OK` (idempotent).
- System actors (where `is_system_actor()` returns true) reject kill with `409`.
- `force=false` may not be supported for all actor types. When not supported,
  the server returns `400 Bad Request` with code `FORCE_NOT_SUPPORTED`.

---

#### `GET /api/v1/actors/{actor_id}/mailbox`

Get a detailed mailbox snapshot for an actor.

**Response `200 OK`:**
```json
{
  "data": {
    "depth": 3,
    "capacity": 1024,
    "high_priority_depth": 0,
    "pressure_state": "low",
    "pressure_ratio_ppm": 2929,
    "total_enqueued": 15003,
    "total_dequeued": 15000,
    "total_rejected": 0,
    "total_dropped": 0,
    "total_dead_letters": 0,
    "overflow_policy": "dead_letter_queue",
    "queued_bytes": 384,
    "byte_capacity": 65536,
    "rate_limiter": { ... },
    "admission": { ... },
    "delivery": { ... }
  }
}
```

**Notes:**
- This is a subset of the full actor detail response, focused on mailbox state.
- Useful for monitoring and alerting without pulling full actor metadata.

---

#### `GET /api/v1/actors/{actor_id}/children`

List an actor's child actors.

**Response `200 OK`:**
```json
{
  "data": [
    {
      "actor_id": 43,
      "actor_type": "HttpConnection",
      "state": "running"
    },
    {
      "actor_id": 44,
      "actor_type": "HttpConnection",
      "state": "idle"
    }
  ],
  "pagination": {
    "offset": 0,
    "limit": 50,
    "total": 2
  }
}
```

---

#### `GET /api/v1/actors/{actor_id}/circuit-breaker`

Get circuit breaker state for an actor.

**Response `200 OK`:**
```json
{
  "data": {
    "state": "half_open",
    "trip_count": 3,
    "failure_ema": 0.45,
    "opened_at_ms": 0
  }
}
```

**Notes:**
- `state` is one of: `"closed"`, `"open"`, `"half_open"`.
- `opened_at_ms` is `0` when the circuit is not currently open.
- Returns `404` if circuit breaker is not configured for this actor.

---

#### `POST /api/v1/actors/{actor_id}/circuit-breaker/reset`

Reset the circuit breaker for an actor (transition from `open`/`half_open` to
`closed`).

**Response `200 OK`:**
```json
{
  "data": {
    "success": true
  }
}
```

**Response `404 Not Found`:**
```json
{
  "error": {
    "code": "CIRCUIT_BREAKER_NOT_CONFIGURED",
    "message": "Actor 42 does not have a circuit breaker configured"
  }
}
```

---

#### `POST /api/v1/actors/{actor_id}/quarantine`

Quarantine an actor. The actor stops processing messages and is isolated from
the system.

**Request Body:**
```json
{
  "reason": "Suspected memory corruption in MetricsActor"
}
```

**Response `200 OK`:**
```json
{
  "data": {
    "success": true
  }
}
```

**Response `400 Bad Request`:**
```json
{
  "error": {
    "code": "QUARANTINE_NOT_ENABLED",
    "message": "Actor 42 does not have quarantine enabled in its configuration"
  }
}
```

**Notes:**
- `reason` is operator-provided text for audit trail and debugging.
- Quarantining an already-quarantined actor is idempotent (returns `200 OK`).

---

#### `DELETE /api/v1/actors/{actor_id}/quarantine`

Release an actor from quarantine.

**Response `200 OK`:**
```json
{
  "data": {
    "success": true
  }
}
```

**Notes:**
- Releasing a non-quarantined actor is idempotent.

---

#### `GET /api/v1/actors/{actor_id}/memory`

Get per-actor memory statistics.

**Response `200 OK`:**
```json
{
  "data": {
    "active_bytes": 1048576,
    "peak_bytes": 2097152,
    "segment_count": 3,
    "slab_hit_rate": 0.95
  }
}
```

---

### 2. System

#### `GET /api/v1/system`

Get a high-level system overview.

**Response `200 OK`:**
```json
{
  "data": {
    "total_actors": 142,
    "running_actors": 138,
    "idle_actors": 4,
    "worker_count": 8,
    "scheduler_utilization": 0.65,
    "uptime_ms": 86400000
  }
}
```

---

#### `GET /api/v1/system/stats`

Get detailed system statistics.

**Response `200 OK`:**
```json
{
  "data": {
    "total_actors": 142,
    "running_actors": 138,
    "idle_actors": 4,
    "worker_count": 8,
    "scheduler_utilization": 0.65,
    "memory_active_bytes": 536870912,
    "uptime_ms": 86400000
  }
}
```

**Notes:**
- This endpoint provides the same data as `GET /api/v1/system` but may include
  additional fields in the future. Use `/system/stats` for programmatic
  consumption and `/system` for a human-readable overview in tools.

---

#### `GET /api/v1/system/memory`

Get system-wide memory statistics.

**Query Parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `actor_id` | integer | — | If set, returns per-actor memory stats (equivalent to `GET /api/v1/actors/{actor_id}/memory`). If omitted, returns system-wide. |

**Response `200 OK` (system-wide):**
```json
{
  "data": {
    "regions": [
      {
        "region": "actor",
        "active_bytes": 104857600,
        "limit_bytes": 536870912,
        "pressure": "low",
        "alloc_count": 50000,
        "free_count": 48000,
        "corruption_events": 0
      },
      {
        "region": "message",
        "active_bytes": 52428800,
        "limit_bytes": 268435456,
        "pressure": "low",
        "alloc_count": 200000,
        "free_count": 195000,
        "corruption_events": 0
      },
      {
        "region": "coroutine",
        "active_bytes": 16777216,
        "limit_bytes": 134217728,
        "pressure": "normal",
        "alloc_count": 8000,
        "free_count": 7500,
        "corruption_events": 0
      },
      {
        "region": "network",
        "active_bytes": 8388608,
        "limit_bytes": 67108864,
        "pressure": "low",
        "alloc_count": 12000,
        "free_count": 11800,
        "corruption_events": 0
      },
      {
        "region": "internal",
        "active_bytes": 4194304,
        "limit_bytes": 33554432,
        "pressure": "low",
        "alloc_count": 1500,
        "free_count": 1400,
        "corruption_events": 0
      },
      {
        "region": "hibernate",
        "active_bytes": 0,
        "limit_bytes": 268435456,
        "pressure": "low",
        "alloc_count": 0,
        "free_count": 0,
        "corruption_events": 0
      }
    ]
  }
}
```

---

#### `POST /api/v1/system/drain`

Initiate system drain. Actors stop accepting new messages but continue
processing in-flight work. This is Step 1 of graceful shutdown.

**Response `202 Accepted`:**
```json
{
  "data": {
    "success": true,
    "message": "System drain initiated"
  }
}
```

**Notes:**
- This is an asynchronous operation. The response returns immediately.
- Monitor `GET /api/v1/system` to observe drain progress (running/idle counts).
- Once drain completes, `POST /api/v1/system/shutdown` to finalize.

---

#### `POST /api/v1/system/shutdown`

Initiate full system shutdown. Drains first, then stops all actors, tears down
scheduler and network resources. Irreversible.

**Response `202 Accepted`:**
```json
{
  "data": {
    "success": true,
    "message": "System shutdown initiated"
  }
}
```

---

### 3. Fault Injection

#### `GET /api/v1/faults`

Get fault injection system status.

**Response `200 OK`:**
```json
{
  "data": {
    "enabled": true,
    "seed": 12345,
    "hooks_triggered": 42,
    "domains": [
      {
        "domain": "mailbox",
        "tick": 10000,
        "active_points": 2
      },
      {
        "domain": "transport",
        "tick": 5000,
        "active_points": 0
      }
    ]
  }
}
```

**Notes:**
- When fault injection is disabled, `enabled` is `false` and `seed`/`hooks_triggered` are `0`.
- The `domains` array lists each fault domain with its current tick count and
  number of active (scheduled but not yet fired) fault points.

---

#### `POST /api/v1/faults/clear`

Clear all fault injection state (reset counters, clear pending fault points).

**Response `200 OK`:**
```json
{
  "data": {
    "success": true
  }
}
```

---

### 4. Dead Letter Queue

#### `GET /api/v1/dead-letter-queue`

List dead letter queue records.

**Query Parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `offset` | integer | 0 | Pagination offset |
| `limit` | integer | 50 | Max results (1–200) |
| `actor_id` | integer | — | Filter by target actor ID |

**Response `200 OK`:**
```json
{
  "data": [
    {
      "index": 0,
      "target_actor_id": 42,
      "reason": "mailbox_full",
      "source": "overflow_handler",
      "type_tag": 7,
      "timestamp_ms": 1700000000000,
      "payload_size_bytes": 256
    }
  ],
  "pagination": {
    "offset": 0,
    "limit": 50,
    "total": 5
  }
}
```

**Response `200 OK` (DLQ not configured):**
```json
{
  "data": [],
  "pagination": {
    "offset": 0,
    "limit": 50,
    "total": 0
  }
}
```

**Notes:**
- When DLQ is not configured, returns an empty list (not a 404). This allows
  clients to handle the absence of DLQ gracefully.
- `payload_size_bytes` is the size of the stored payload sample. The full
  payload content is available via `GET /dead-letter-queue/{index}`.

---

#### `GET /api/v1/dead-letter-queue/{index}`

Get a specific dead letter queue record by its 0-based index.

**Response `200 OK`:**
```json
{
  "data": {
    "index": 0,
    "target_actor_id": 42,
    "reason": "mailbox_full",
    "source": "overflow_handler",
    "type_tag": 7,
    "timestamp_ms": 1700000000000,
    "payload_size_bytes": 256
  }
}
```

**Response `404 Not Found`:**
```json
{
  "error": {
    "code": "DLQ_INDEX_OUT_OF_RANGE",
    "message": "DLQ index 99 is out of range (0 records available)"
  }
}
```

---

#### `POST /api/v1/dead-letter-queue/{index}/replay`

Replay a dead letter queue record to a target actor.

**Request Body:**
```json
{
  "target_actor_id": 99
}
```

`target_actor_id` is optional. When omitted, the message is replayed to its
original target actor.

**Response `200 OK`:**
```json
{
  "data": {
    "success": true
  }
}
```

**Response `409 Conflict`:**
```json
{
  "error": {
    "code": "REPLAY_DELIVERY_FAILED",
    "message": "Delivery to actor 99 failed: mailbox full"
  }
}
```

---

#### `GET /api/v1/dead-letter-queue/export`

Export all dead letter queue records.

**Query Parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `actor_id` | integer | — | Filter by target actor ID |

**Response `200 OK`:**
```json
{
  "data": [ ... ]
}
```

**Notes:**
- Returns the full list of records (up to the DLQ capacity). No pagination —
  this endpoint is for bulk export to external systems.
- The response shape is identical to `GET /dead-letter-queue` but includes all
  records. The `pagination` envelope is omitted.

---

### 5. Asks

#### `GET /api/v1/asks`

List pending ask (request-response) operations.

**Query Parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `offset` | integer | 0 | Pagination offset |
| `limit` | integer | 50 | Max results (1–200) |

**Response `200 OK`:**
```json
{
  "data": [
    {
      "message_id": 12345,
      "source_actor_id": 10,
      "target_actor_id": 42,
      "deadline_ms": 1700000005000,
      "retries_remaining": 2
    }
  ],
  "pagination": {
    "offset": 0,
    "limit": 50,
    "total": 3
  }
}
```

**Notes:**
- `deadline_ms` is an absolute system-clock timestamp in milliseconds.
- Returns an empty list if the ask subsystem is not configured.

---

#### `GET /api/v1/asks/{message_id}`

Get details for a specific pending ask by its message ID.

**Response `200 OK`:**
```json
{
  "data": {
    "message_id": 12345,
    "source_actor_id": 10,
    "target_actor_id": 42,
    "deadline_ms": 1700000005000,
    "retries_remaining": 2
  }
}
```

**Response `404 Not Found`:**
```json
{
  "error": {
    "code": "ASK_NOT_FOUND",
    "message": "No pending ask with message_id 12345"
  }
}
```

---

#### `POST /api/v1/asks/{message_id}/cancel`

Cancel a pending ask by its message ID. The requesting actor receives a timeout
error for the cancelled ask.

**Response `200 OK`:**
```json
{
  "data": {
    "success": true
  }
}
```

**Notes:**
- Cancelling an already-completed or non-existent ask is idempotent (returns
  `200 OK`).

---

### 6. Endpoints (Future)

The following endpoints are reserved for a future release. They will provide
visibility into network endpoints, connection pools, and circuit breaker state
for remote actor communication. Implementation is deferred until the
`ISystemCliHost` interface exposes structured endpoint data.

```
GET    /api/v1/endpoints                    List network endpoints
GET    /api/v1/endpoints/{endpoint_id}      Get endpoint details
POST   /api/v1/endpoints/{endpoint_id}/circuit-breaker/reset   Reset endpoint circuit
```

---

### 7. API Index

#### `GET /api/v1/`

Returns a JSON index of all available API endpoints for discoverability.

**Response `200 OK`:**
```json
{
  "data": {
    "version": "v1",
    "endpoints": {
      "actors": "/api/v1/actors",
      "system": "/api/v1/system",
      "system_stats": "/api/v1/system/stats",
      "system_memory": "/api/v1/system/memory",
      "system_drain": "/api/v1/system/drain",
      "system_shutdown": "/api/v1/system/shutdown",
      "faults": "/api/v1/faults",
      "dead_letter_queue": "/api/v1/dead-letter-queue",
      "asks": "/api/v1/asks"
    }
  }
}
```

---

## Data Types

### ActorMetadata

| Field | Type | Description |
|-------|------|-------------|
| `actor_id` | integer (uint64) | Unique actor identifier |
| `actor_type` | string | Actor class name (e.g., `"MetricsActor"`) |
| `state` | string | Lifecycle state: `"created"`, `"starting"`, `"running"`, `"idle"`, `"stopping"`, `"stopped"`, `"failed"`, `"terminated"` |
| `incarnation` | integer (uint64) | Monotonically increasing restart counter |
| `messages_processed` | integer (uint64) | Total messages processed since creation |
| `uptime_ms` | integer (uint64) | Milliseconds since actor creation (monotonic) |
| `behavior_name` | string | Current behavior name (e.g., `"metrics_behavior"`) |

### MailboxSnapshot

| Field | Type | Description |
|-------|------|-------------|
| `depth` | integer (uint32) | Current number of messages in the mailbox |
| `capacity` | integer (uint32) | Maximum number of messages (0 = unbounded) |
| `high_priority_depth` | integer (uint32) | Messages in the high-priority system lane |
| `pressure_state` | string | `"low"`, `"high"`, or `"critical"` |
| `pressure_ratio_ppm` | integer (uint32) | Depth/capacity ratio in parts-per-million |
| `total_enqueued` | integer (uint64) | Lifetime enqueue count |
| `total_dequeued` | integer (uint64) | Lifetime dequeue count |
| `max_depth` | integer (uint64) | Historical maximum depth |
| `total_rejected` | integer (uint64) | Messages rejected at admission |
| `total_dropped` | integer (uint64) | Messages dropped by overflow handler |
| `total_dead_letters` | integer (uint64) | Messages routed to DLQ |
| `overflow_policy` | string | Active overflow policy name |
| `queued_bytes` | integer (uint64) | Current bytes in the mailbox |
| `byte_capacity` | integer (uint64) | Maximum bytes (0 = unbounded) |
| `rate_limiter` | object | Rate limiter state (see below) |
| `admission` | object | Admission control stats (see below) |
| `delivery` | object | Delivery outcome stats (see below) |

#### RateLimiter (nested in MailboxSnapshot)

| Field | Type | Description |
|-------|------|-------------|
| `enabled` | boolean | Whether rate limiting is active |
| `rate` | number (double) | Token refill rate per second |
| `burst` | integer (uint32) | Maximum token bucket size |
| `current_tokens` | number (double) | Current token count |
| `blocked_total` | integer (uint64) | Messages blocked by rate limiter |

#### Admission (nested in MailboxSnapshot)

| Field | Type | Description |
|-------|------|-------------|
| `policy_count` | integer (uint32) | Number of admission policies active |
| `rejected_total` | integer (uint64) | Messages rejected by admission |
| `dlq_routed_total` | integer (uint64) | Messages routed to DLQ by admission |

#### Delivery (nested in MailboxSnapshot)

| Field | Type | Description |
|-------|------|-------------|
| `accepted_total` | integer (uint64) | Messages successfully delivered |
| `rejected_total` | integer (uint64) | Messages rejected at delivery |
| `failed_total` | integer (uint64) | Messages that failed after acceptance |
| `retryable_total` | integer (uint64) | Messages queued for retry |

### ChildInfo

| Field | Type | Description |
|-------|------|-------------|
| `actor_id` | integer (uint64) | Child actor identifier |
| `actor_type` | string | Child actor class name |
| `state` | string | Child lifecycle state |

### CircuitBreakerInfo

| Field | Type | Description |
|-------|------|-------------|
| `state` | string | `"closed"`, `"open"`, or `"half_open"` |
| `trip_count` | integer (uint32) | Number of times the circuit has tripped |
| `failure_ema` | number (double) | Exponential moving average of failure rate |
| `opened_at_ms` | integer (uint64) | Monotonic timestamp when circuit opened (0 if closed) |

### DLQRecord

| Field | Type | Description |
|-------|------|-------------|
| `index` | integer (uint32) | 0-based index in the DLQ ring buffer |
| `target_actor_id` | integer (uint64) | Intended recipient actor |
| `reason` | string | Failure reason code (e.g., `"mailbox_full"`) |
| `source` | string | Failure source subsystem (e.g., `"overflow_handler"`) |
| `type_tag` | integer (uint32) | Protobuf type tag of the dead-lettered message |
| `timestamp_ms` | integer (uint64) | System-clock timestamp in milliseconds |
| `payload_size_bytes` | integer (uint64) | Size of the stored payload sample |

### PendingAsk

| Field | Type | Description |
|-------|------|-------------|
| `message_id` | integer (uint64) | Correlation message ID |
| `source_actor_id` | integer (uint64) | Actor that issued the ask |
| `target_actor_id` | integer (uint64) | Actor expected to respond |
| `deadline_ms` | integer (uint64) | Absolute system-clock deadline in ms |
| `retries_remaining` | integer (uint32) | Remaining retry attempts before timeout |

### ErrorResponse

| Field | Type | Description |
|-------|------|-------------|
| `error.code` | string | Machine-readable error code (see [Error Codes](#error-codes)) |
| `error.message` | string | Human-readable error description |

---

## Error Codes

| Code | HTTP Status | Description |
|------|-------------|-------------|
| `ACTOR_NOT_FOUND` | 404 | The specified actor does not exist |
| `ACTOR_NOT_STOPPABLE` | 409 | The actor is a system actor and cannot be killed |
| `FORCE_NOT_SUPPORTED` | 400 | Graceful stop is not supported for this actor type |
| `QUARANTINE_NOT_ENABLED` | 400 | Quarantine is not configured for this actor |
| `CIRCUIT_BREAKER_NOT_CONFIGURED` | 404 | Circuit breaker is not configured for this actor |
| `DLQ_INDEX_OUT_OF_RANGE` | 404 | The specified DLQ index does not exist |
| `DLQ_NOT_CONFIGURED` | 404 | The dead letter queue subsystem is not configured |
| `REPLAY_DELIVERY_FAILED` | 409 | Replayed message could not be delivered |
| `ASK_NOT_FOUND` | 404 | No pending ask with the specified message ID |
| `ASK_CANCEL_FAILED` | 409 | The ask could not be cancelled (already completed) |
| `SYSTEM_NOT_READY` | 503 | The actor system is starting up and not accepting commands |
| `SYSTEM_DRAINING` | 503 | The system is draining; new operations are rejected |
| `INVALID_JSON` | 400 | Request body is not valid JSON |
| `INVALID_FIELD` | 400 | A field value is invalid (see message for details) |
| `MISSING_FIELD` | 400 | A required field is missing |
| `INVALID_PAGINATION` | 400 | offset/limit values are out of range |
| `INTERNAL_ERROR` | 500 | An unexpected internal error occurred |

---

## Client Implementation Guide

This section provides guidance for implementing an HTTP-based CLI client in any
language.

### Base URL

The API is served at `http://{host}:{port}/api/v1/`. By default, the host is
`127.0.0.1` and the port is `9090` (configurable via
`[system.cli.http]` TOML section).

### Request Headers

All requests should include:
```
Accept: application/json
Content-Type: application/json    (for POST requests)
```

### Response Handling

A minimal client implementation should:

1. Check the HTTP status code first.
2. On `2xx`, parse `data` from the JSON body.
3. On `4xx`/`5xx`, parse `error.code` and `error.message` from the JSON body.
4. Handle `pagination` in collection responses to iterate through large result
   sets.

### Pseudocode Example

```python
import requests

BASE = "http://127.0.0.1:9090/api/v1"

def get_actor(actor_id: int, fields: list[str] | None = None):
    url = f"{BASE}/actors/{actor_id}"
    params = {}
    if fields:
        params["fields"] = ",".join(fields)
    resp = requests.get(url, params=params,
                        headers={"Accept": "application/json"})
    if resp.status_code == 200:
        return resp.json()["data"]
    elif resp.status_code == 404:
        raise ActorNotFound(resp.json()["error"]["message"])
    else:
        raise ApiError(resp.json()["error"]["code"],
                       resp.json()["error"]["message"])

def list_actors(actor_type: str | None = None, offset: int = 0, limit: int = 50):
    url = f"{BASE}/actors"
    params = {"offset": offset, "limit": limit}
    if actor_type:
        params["actor_type"] = actor_type
    resp = requests.get(url, params=params,
                        headers={"Accept": "application/json"})
    if resp.status_code == 200:
        body = resp.json()
        return body["data"], body["pagination"]
    else:
        raise ApiError(...)

def kill_actor(actor_id: int, force: bool = True):
    url = f"{BASE}/actors/{actor_id}"
    params = {"force": str(force).lower()}
    resp = requests.delete(url, params=params,
                           headers={"Accept": "application/json"})
    if resp.status_code == 200:
        return resp.json()["data"]["success"]
    elif resp.status_code == 409:
        raise ActorNotStoppable(resp.json()["error"]["message"])
    else:
        raise ApiError(...)

def replay_dlq(index: int, target_actor_id: int | None = None):
    url = f"{BASE}/dead-letter-queue/{index}/replay"
    body = {}
    if target_actor_id is not None:
        body["target_actor_id"] = target_actor_id
    resp = requests.post(url, json=body,
                         headers={"Accept": "application/json"})
    if resp.status_code == 200:
        return resp.json()["data"]["success"]
    else:
        raise ApiError(...)

# Iterate all actors with pagination
def list_all_actors():
    offset = 0
    limit = 50
    while True:
        data, pagination = list_actors(offset=offset, limit=limit)
        for actor in data:
            yield actor
        if offset + limit >= pagination["total"]:
            break
        offset += limit
```

### Error Handling Best Practices

1. **Always check HTTP status codes** — don't rely on the presence/absence of
   `error` in the body.
2. **Log `error.code`** for programmatic alerting and dashboards.
3. **Display `error.message`** to the user for troubleshooting.
4. **Retry on `503`** with exponential backoff (system starting/draining).
5. **Do not retry on `4xx`** without fixing the request.
6. **Retry on `500`** with backoff (transient internal errors).

### Pagination

All collection endpoints use offset-based pagination:

1. Start with `?offset=0&limit=50`.
2. Check `pagination.total` to know the total count.
3. Increment offset by limit until `offset + limit >= total`.
4. Respect the server's `limit` cap (200). If you request `limit=500`, the
   server returns at most 200 results.

### Field Selection

Detail endpoints support `?fields=` to reduce response size:

```
GET /api/v1/actors/42?fields=metadata,children
```

The response includes only the requested top-level keys:
```json
{
  "data": {
    "metadata": { ... },
    "children": [ ... ]
  }
}
```

Time-series monitoring clients should use `?fields=mailbox` when they only need
pressure/depth metrics, avoiding the overhead of metadata, children, and circuit
breaker data.

---

## Migration from `POST /cli`

### Deprecation Timeline

1. **Phase 1 (current release)**: Deploy the REST API alongside the existing
   `POST /cli` endpoint. Both paths are served by `CliHttpServerActor`.
2. **Phase 2 (next release)**: Add `Deprecation: true` and `Sunset: <date>`
   headers to `POST /cli` responses. Log deprecation warnings.
3. **Phase 3 (release after)**: Remove `POST /cli` support.

### Legacy Command Mapping

For clients migrating from the current `POST /cli` JSON format, here is the
mapping from CLI paths to REST endpoints:

| CLI Path (`POST /cli`) | REST Endpoint |
|------------------------|---------------|
| `{"path": "actor/42/show"}` | `GET /api/v1/actors/42` |
| `{"path": "actor/42/show", "params": {"--json": "true"}}` | `GET /api/v1/actors/42` (all responses are JSON) |
| `{"path": "actor/42/kill"}` | `DELETE /api/v1/actors/42` |
| `{"path": "actor/42/kill", "params": {"--force": "true"}}` | `DELETE /api/v1/actors/42?force=true` |
| `{"path": "actor/list"}` | `GET /api/v1/actors` |
| `{"path": "actor/list", "params": {"--type": "MetricsActor"}}` | `GET /api/v1/actors?actor_type=MetricsActor` |
| `{"path": "system/stats"}` | `GET /api/v1/system/stats` |
| `{"path": "system/memory"}` | `GET /api/v1/system/memory` |
| `{"path": "system/drain"}` | `POST /api/v1/system/drain` |
| `{"path": "system/shutdown"}` | `POST /api/v1/system/shutdown` |
| `{"path": "fault/status"}` | `GET /api/v1/faults` |
| `{"path": "fault/clear"}` | `POST /api/v1/faults/clear` |
| `{"path": "dlq/list"}` | `GET /api/v1/dead-letter-queue` |
| `{"path": "dlq/list", "params": {"--actor": "42"}}` | `GET /api/v1/dead-letter-queue?actor_id=42` |
| `{"path": "dlq/show", "args": ["0"]}` | `GET /api/v1/dead-letter-queue/0` |
| `{"path": "dlq/replay", "args": ["0", "99"]}` | `POST /api/v1/dead-letter-queue/0/replay` with `{"target_actor_id": 99}` |
| `{"path": "dlq/export"}` | `GET /api/v1/dead-letter-queue/export` |
| `{"path": "ask/pending"}` | `GET /api/v1/asks` |
| `{"path": "ask/cancel", "args": ["12345"]}` | `POST /api/v1/asks/12345/cancel` |
| `{"path": "actor/42/quarantine", "params": {"--reason": "test"}}` | `POST /api/v1/actors/42/quarantine` with `{"reason": "test"}` |
| `{"path": "actor/42/unquarantine"}` | `DELETE /api/v1/actors/42/quarantine` |

---

## Implementation Plan

### Required Changes to `CliHttpServerActor`

The current `CliHttpServerActor::on_http_request()` method (single `POST /cli`
handler) must be replaced with a routing dispatcher that maps HTTP method + path
to handler functions.

#### 1. New Dependencies

The HTTP server will need to implement `ICliCommandHost` for direct actor
operations:

```cpp
class CliHttpServerActor : public DaemonActor,
                           public ICliCommandHost,      // NEW
                           public ISystemCliHost,
                           public ILifecycleCliHost {
```

This requires implementing:
- `inspect(ActorId, const InspectStateRequest&, milliseconds)` → returns `InspectStateReply`
- `kill(ActorId, const KillRequest&, milliseconds)` → returns `KillReply`
- `quarantine(ActorId, const QuarantineRequest&, milliseconds)` → returns `QuarantineReply`
- `enumerate(string_view)` → returns `vector<ActorMeta>`

These methods already exist in `CliLocalActor` and `CliClientActor`. The HTTP
server implementation can follow the `CliLocalActor` pattern (direct
`try_deliver_local` + `wait_for_response` or synchronous access to actor state).

#### 2. Routing Architecture

Replace the monolithic `on_http_request()` with a routing table:

```
GET    /api/v1/                                          → handle_api_index()
GET    /api/v1/actors                                    → handle_list_actors()
GET    /api/v1/actors/{id}                               → handle_get_actor()
DELETE /api/v1/actors/{id}                               → handle_kill_actor()
GET    /api/v1/actors/{id}/mailbox                       → handle_get_mailbox()
GET    /api/v1/actors/{id}/children                      → handle_get_children()
GET    /api/v1/actors/{id}/circuit-breaker               → handle_get_circuit_breaker()
POST   /api/v1/actors/{id}/circuit-breaker/reset         → handle_reset_circuit_breaker()
POST   /api/v1/actors/{id}/quarantine                    → handle_quarantine()
DELETE /api/v1/actors/{id}/quarantine                    → handle_unquarantine()
GET    /api/v1/actors/{id}/memory                        → handle_get_actor_memory()
GET    /api/v1/system                                    → handle_get_system()
GET    /api/v1/system/stats                              → handle_get_system_stats()
GET    /api/v1/system/memory                             → handle_get_system_memory()
POST   /api/v1/system/drain                              → handle_drain()
POST   /api/v1/system/shutdown                           → handle_shutdown()
GET    /api/v1/faults                                    → handle_get_faults()
POST   /api/v1/faults/clear                              → handle_clear_faults()
GET    /api/v1/dead-letter-queue                         → handle_list_dlq()
GET    /api/v1/dead-letter-queue/{index}                 → handle_get_dlq_record()
POST   /api/v1/dead-letter-queue/{index}/replay          → handle_replay_dlq()
GET    /api/v1/dead-letter-queue/export                  → handle_export_dlq()
GET    /api/v1/asks                                      → handle_list_asks()
GET    /api/v1/asks/{message_id}                         → handle_get_ask()
POST   /api/v1/asks/{message_id}/cancel                  → handle_cancel_ask()
```

The `net::HTTPGateway`'s `RouteRegistry` already supports path parameter
extraction via `:param` patterns. Use it for route matching rather than manual
path parsing. Example:

```cpp
gateway_->route(net::HttpMethod::GET, "/api/v1/actors/:actor_id",
                [this](auto* conn, auto&& req) { ... });
gateway_->route(net::HttpMethod::GET, "/api/v1/actors/:actor_id/mailbox",
                [this](auto* conn, auto&& req) { ... });
```

**Important:** Register routes in priority order. Literal paths (`/api/v1/actors`)
must be registered before parameterized paths (`/api/v1/actors/:actor_id`) to
ensure correct dispatch.

#### 3. JSON Serialization

Replace the hand-rolled `parse_cli_command_json` / `serialize_cli_response_json`
helpers with structured serialization for each endpoint's specific response type.

Since the project avoids exceptions and RTTI, a manual serialization approach is
appropriate:

```cpp
// Helper: build JSON string for a response
std::string serialize_actor_metadata(const InspectStateReply& reply);
std::string serialize_mailbox_snapshot(const MailboxSnapshot& snap);
std::string serialize_pagination(uint32_t offset, uint32_t limit, uint32_t total);
std::string serialize_error(const std::string& code, const std::string& message);
```

These replace the current `serialize_cli_response_json()` which wraps everything
in a `CliResponse` envelope.

A structured approach using a `JsonBuilder` helper class is recommended:

```cpp
class JsonBuilder {
public:
    JsonBuilder& begin_object();
    JsonBuilder& end_object();
    JsonBuilder& begin_array();
    JsonBuilder& end_array();
    JsonBuilder& key(const std::string& k);
    JsonBuilder& value(const std::string& v);
    JsonBuilder& value(uint64_t v);
    JsonBuilder& value(int64_t v);
    JsonBuilder& value(uint32_t v);
    JsonBuilder& value(double v);
    JsonBuilder& value(bool v);
    std::string build();
};
```

This avoids string concatenation overhead and produces valid JSON with proper
escaping.

#### 4. Error Handling

Every handler follows the same error pattern:

```cpp
void handle_get_actor(net::HTTPConnection* conn, net::HttpRequest&& req) {
    uint64_t actor_id = parse_actor_id(req.path_params["actor_id"]);
    if (actor_id == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest,
                   "INVALID_FIELD", "actor_id must be a positive integer");
        return;
    }

    auto reply = inspect(ActorId{actor_id}, build_inspect_request(req));
    if (!reply.has_value()) {
        send_error(conn, net::HttpStatusCode::NotFound,
                   "ACTOR_NOT_FOUND",
                   "Actor with id " + std::to_string(actor_id) + " does not exist");
        return;
    }

    std::string json = build_actor_response(reply.value(), req.query_params);
    send_json(conn, net::HttpStatusCode::OK, json);
}
```

#### 5. Query Parameter Parsing

Extract query parameter helpers:

```cpp
std::optional<uint32_t> parse_offset(const net::HttpRequest& req);
std::optional<uint32_t> parse_limit(const net::HttpRequest& req);   // clamped to [1, 200]
std::optional<std::string> parse_filter(const net::HttpRequest& req, const std::string& key);
std::vector<std::string> parse_fields(const net::HttpRequest& req);
```

#### 6. Backward Compatibility

During Phase 1, delegate `POST /cli` to a legacy handler that preserves the
existing behavior:

```cpp
// Phase 1 only — remove in Phase 3
gateway_->route(net::HttpMethod::POST, "/cli",
                [this](auto* conn, auto&& req) {
                    handle_legacy_post_cli(conn, std::move(req));
                });
```

The legacy handler extracts the existing `parse_cli_command_json()` +
`CliSession` dispatch logic, unchanged from the current implementation.

### Test Plan

| Test Suite | Scope | New Tests |
|------------|-------|-----------|
| `test_cli_http_server` | Unit: routing, JSON serde, error handling | 20+ (one per endpoint + error cases) |
| `test_cli_http_api` | Integration: end-to-end HTTP requests | 15+ (happy path per resource) |
| `test_cli_http_migration` | Integration: legacy `POST /cli` compatibility | 5+ (existing behavior preserved) |

Test approach:
- Unit tests: mock `ActorSystem`/`ICliCommandHost`, verify JSON output
- Integration tests: real `ActorSystem` with test actors, verify HTTP responses
- Use the existing `test_cli_http_server.cpp` as a starting point

### Files to Create/Modify

| Action | File | Description |
|--------|------|-------------|
| **MODIFY** | `src/cli/cli_http_server_actor.cpp` | Replace single-handler with routing table |
| **MODIFY** | `include/hpactor/cli/cli_http_server_actor.hpp` | Add `ICliCommandHost`, new handler methods |
| **ADD** | `src/cli/json_builder.cpp` | JSON serialization helper |
| **ADD** | `include/hpactor/cli/json_builder.hpp` | JSON serialization helper header |
| **ADD** | `src/cli/cli_http_handlers.cpp` | Per-endpoint handler implementations |
| **MODIFY** | `tests/unit/cli/test_cli_http_server.cpp` | Rewrite for REST endpoints |
| **ADD** | `tests/integration/cli/test_cli_http_api.cpp` | End-to-end HTTP integration tests |

### Configuration

Add a new TOML config section for the HTTP API:

```toml
[system.cli.http]
port = 9090
bind_address = "127.0.0.1"
max_connections = 100
legacy_cli_endpoint = true   # Enable POST /cli backward compat (Phase 1-2)
```

The existing `CliHttpServerConfig` already has `http_port`, `http_bind_address`,
and `max_connections`. Add `legacy_cli_endpoint` (default `true` for Phase 1).

---

## References

- [Postman REST API Best Practices](https://blog.postman.com/rest-api-best-practices/)
- [RFC 7231 — HTTP/1.1 Semantics and Content](https://datatracker.ietf.org/doc/html/rfc7231)
- [Microsoft REST API Guidelines](https://github.com/microsoft/api-guidelines/blob/vNext/Guidelines.md)
- [Google API Design Guide](https://cloud.google.com/apis/design)
- PR #307: CLI Architecture Standardization — introduced `ICliCommandHost`,
  `CliClientActor`, `CliProtoServerActor`, and the `CliCommand`/`CliResponse`
  wire protocol.
- `docs/architecture/cli/cli-architecture-core-concept-and-high-level-design.md`
- `docs/architecture/cli/cli-architecture-detailed-design.md`
