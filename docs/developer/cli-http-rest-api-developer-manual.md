# HPActor REST API — Developer Manual

<!--
Copyright 2026 HPActor Contributors
SPDX-License-Identifier: Apache-2.0
-->

## Quick Start

The HPActor REST API provides programmatic access to inspect and manage a
running actor system.  It is served by `CliHttpServerActor` on
`http://127.0.0.1:9090/api/v1` by default.

```bash
# Is the API alive?
curl http://127.0.0.1:9090/api/v1/

# List all actors
curl http://127.0.0.1:9090/api/v1/actors

# Inspect a specific actor
curl http://127.0.0.1:9090/api/v1/actors/42

# Get just the mailbox for monitoring
curl "http://127.0.0.1:9090/api/v1/actors/42?fields=mailbox"

# Get system stats
curl http://127.0.0.1:9090/api/v1/system/stats

# Kill an actor
curl -X DELETE http://127.0.0.1:9090/api/v1/actors/42

# Initiate graceful shutdown
curl -X POST http://127.0.0.1:9090/api/v1/system/drain
```

## OpenAPI Specification

A machine-readable [OpenAPI 3.0 specification](cli-http-rest-api-openapi.yaml)
is provided alongside this document.  Use it to generate type-safe client
libraries:

```bash
# Generate a Python client
openapi-generator generate -i cli-http-rest-api-openapi.yaml -g python -o client/

# Generate a Go client
openapi-generator generate -i cli-http-rest-api-openapi.yaml -g go -o client/

# Generate a TypeScript client
openapi-generator generate -i cli-http-rest-api-openapi.yaml -g typescript-fetch -o client/
```

## Response Envelope

Every response follows one of four shapes:

### Success — Single Item
```json
{
  "data": { ... }
}
```

### Success — Collection
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

### Success — Action Result
```json
{
  "data": {
    "success": true
  }
}
```

### Success — Async Accepted
```json
{
  "data": {
    "success": true,
    "message": "System drain initiated"
  }
}
```

### Error
```json
{
  "error": {
    "code": "ACTOR_NOT_FOUND",
    "message": "Actor 42 not found or not responding"
  }
}
```

Client code should check the HTTP status code first, then the presence of
`error` vs `data` in the body:

```python
def handle_response(resp):
    if resp.status_code == 200:
        return resp.json()["data"]
    elif resp.status_code == 202:
        print(f"Accepted: {resp.json()['data']['message']}")
        return None
    elif resp.status_code >= 400:
        err = resp.json()["error"]
        raise ApiError(err["code"], err["message"])
```

## Pagination

All collection endpoints use offset-based pagination.  Iterate until you have
received all items:

```python
def list_all_actors(client, actor_type=None):
    offset = 0
    limit = 50
    while True:
        resp = client.get("/actors", params={
            "offset": offset,
            "limit": limit,
            "actor_type": actor_type
        })
        data = resp["data"]
        pagination = resp["pagination"]
        for actor in data:
            yield actor
        if offset + limit >= pagination["total"]:
            break
        offset += limit
```

**Pagination rules:**
- `offset` defaults to 0, `limit` defaults to 50
- `limit` is clamped to `[1, 200]` — requesting `limit=500` returns at most 200
- `total` reflects the number of items matching the current filter
- If `offset >= total`, the `data` array is empty

## Field Selection

`GET /actors/{actor_id}` accepts `?fields=` to reduce response size:

```bash
# Only metadata and mailbox — no children, circuit breaker, quarantine
curl "http://127.0.0.1:9090/api/v1/actors/42?fields=metadata,mailbox"
```

Available field names:

| Field | Controls |
|-------|----------|
| `metadata` | Actor identity and lifecycle state (always included) |
| `mailbox` | Mailbox depth, pressure, rate limiter, admission, delivery stats |
| `children` | Child actor list |
| `circuit_breaker` | Circuit breaker state, trip count, failure EMA |
| `quarantine` | Quarantine enabled flag, reason |
| `rate_limiter` | Token bucket state (nested under mailbox) |
| `admission` | Admission policy stats (nested under mailbox) |

## Error Codes

| Code | HTTP | Meaning |
|------|------|---------|
| `INVALID_FIELD` | 400 | A path or query parameter is malformed |
| `MISSING_FIELD` | 400 | A required JSON body field is absent |
| `INVALID_JSON` | 400 | Request body is not valid JSON |
| `INVALID_PAGINATION` | 400 | offset/limit values out of range |
| `UNSUPPORTED_MEDIA_TYPE` | 415 | Content-Type is not `application/json` |
| `ACTOR_NOT_FOUND` | 404 | Actor does not exist or is not responding |
| `CIRCUIT_BREAKER_NOT_CONFIGURED` | 404 | Actor has no circuit breaker |
| `DLQ_INDEX_OUT_OF_RANGE` | 404 | DLQ index exceeds record count |
| `DLQ_NOT_CONFIGURED` | 404 | DLQ subsystem not configured |
| `ASK_NOT_FOUND` | 404 | No pending ask with that message_id |
| `NOT_FOUND` | 404 | No route matches the request |
| `ACTOR_NOT_STOPPABLE` | 409 | Kill attempted on a system actor |
| `REPLAY_DELIVERY_FAILED` | 409 | DLQ replay message could not be delivered |
| `NOT_IMPLEMENTED` | 501 | Feature not yet available |
| `INTERNAL_ERROR` | 500 | Unexpected server-side failure |

## Endpoint Reference

### API Index

```
GET /api/v1/
```

Returns a map of all available endpoints.  Always start here — the response
is the canonical list of what the server supports.

### Actors

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/actors` | List actors (paginated, filterable) |
| `GET` | `/actors/{id}` | Actor detail with field selection |
| `DELETE` | `/actors/{id}` | Kill an actor (`?force=false` for graceful) |
| `GET` | `/actors/{id}/mailbox` | Mailbox snapshot |
| `GET` | `/actors/{id}/children` | Child actors |
| `GET` | `/actors/{id}/circuit-breaker` | Circuit breaker state |
| `POST` | `/actors/{id}/circuit-breaker/reset` | Reset circuit breaker *(501 — not yet implemented)* |
| `POST` | `/actors/{id}/quarantine` | Quarantine an actor |
| `DELETE` | `/actors/{id}/quarantine` | Release from quarantine |
| `GET` | `/actors/{id}/memory` | Per-actor memory *(501 — not yet implemented)* |

### System

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/system` | System overview |
| `GET` | `/system/stats` | System statistics |
| `GET` | `/system/memory` | Memory region statistics |
| `POST` | `/system/drain` | Initiate graceful drain |
| `POST` | `/system/shutdown` | Initiate full shutdown |

### Fault Injection

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/faults` | Fault injection status |
| `POST` | `/faults/clear` | Clear fault counters |

### Dead Letter Queue

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/dead-letter-queue` | List DLQ records (paginated) |
| `GET` | `/dead-letter-queue/{index}` | Get a single DLQ record |
| `POST` | `/dead-letter-queue/{index}/replay` | Replay a DLQ record |
| `GET` | `/dead-letter-queue/export` | Export all DLQ records |

### Asks

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/asks` | List pending asks *(501 — not yet implemented)* |
| `GET` | `/asks/{message_id}` | Get ask detail *(501 — not yet implemented)* |
| `POST` | `/asks/{message_id}/cancel` | Cancel an ask *(501 — not yet implemented)* |

## Common Workflows

### Investigate a Misbehaving Actor

```bash
# 1. Find the actor
curl "http://127.0.0.1:9090/api/v1/actors?actor_type=OrderProcessor"

# 2. Inspect its full state
curl "http://127.0.0.1:9090/api/v1/actors/42"

# 3. Check mailbox pressure
curl "http://127.0.0.1:9090/api/v1/actors/42/mailbox"

# 4. Check circuit breaker (is it tripped?)
curl "http://127.0.0.1:9090/api/v1/actors/42/circuit-breaker"

# 5. Check for dead-lettered messages
curl "http://127.0.0.1:9090/api/v1/dead-letter-queue?actor_id=42"

# 6. If the actor needs restarting
curl -X DELETE "http://127.0.0.1:9090/api/v1/actors/42?force=true"
```

### Monitor System Health

```bash
# Poll system stats every 10 seconds
watch -n 10 'curl -s http://127.0.0.1:9090/api/v1/system/stats | jq .'

# Monitor memory pressure
curl -s http://127.0.0.1:9090/api/v1/system/memory | jq '.data.regions[] | {name, pressure, active_bytes}'

# Check DLQ growth
curl -s http://127.0.0.1:9090/api/v1/dead-letter-queue | jq '.pagination.total'
```

### Graceful Shutdown

```bash
# Step 1: Drain (stop accepting new work, finish in-flight)
curl -X POST http://127.0.0.1:9090/api/v1/system/drain

# Step 2: Wait for drain to complete — poll until running_actors == 0
while true; do
    running=$(curl -s http://127.0.0.1:9090/api/v1/system | jq '.data.total_actors')
    if [ "$running" -eq 0 ]; then break; fi
    sleep 1
done

# Step 3: Final shutdown
curl -X POST http://127.0.0.1:9090/api/v1/system/shutdown
```

### Chaos Engineering with Fault Injection

```bash
# Check if fault injection is active
curl http://127.0.0.1:9090/api/v1/faults

# After a chaos experiment, clear the counters
curl -X POST http://127.0.0.1:9090/api/v1/faults/clear
```

### DLQ Triage

```bash
# List all dead-lettered messages
curl http://127.0.0.1:9090/api/v1/dead-letter-queue

# Inspect a specific record
curl http://127.0.0.1:9090/api/v1/dead-letter-queue/0

# Replay it to its original target
curl -X POST http://127.0.0.1:9090/api/v1/dead-letter-queue/0/replay \
  -H "Content-Type: application/json" -d '{}'

# Or replay to a different actor
curl -X POST http://127.0.0.1:9090/api/v1/dead-letter-queue/0/replay \
  -H "Content-Type: application/json" -d '{"target_actor_id": 99}'

# Export all records for offline analysis
curl http://127.0.0.1:9090/api/v1/dead-letter-queue/export > dlq_dump.json
```

## Client Library Examples

### Python

```python
import requests
from typing import Optional, Iterator

class HPActorClient:
    """Minimal HPActor REST API client."""

    def __init__(self, base_url: str = "http://127.0.0.1:9090/api/v1"):
        self.base = base_url
        self.session = requests.Session()
        self.session.headers["Accept"] = "application/json"

    def _get(self, path: str, **params) -> dict:
        resp = self.session.get(f"{self.base}{path}", params=params)
        self._check(resp)
        return resp.json()

    def _post(self, path: str, body: Optional[dict] = None) -> dict:
        resp = self.session.post(
            f"{self.base}{path}",
            json=body or {},
            headers={"Content-Type": "application/json"}
        )
        self._check(resp)
        return resp.json()

    def _delete(self, path: str, **params) -> dict:
        resp = self.session.delete(f"{self.base}{path}", params=params)
        self._check(resp)
        return resp.json()

    @staticmethod
    def _check(resp):
        if resp.status_code >= 400:
            err = resp.json().get("error", {})
            raise RuntimeError(f"{err.get('code', 'UNKNOWN')}: {err.get('message', '')}")

    # ── Actors ─────────────────────────────────────────────────

    def list_actors(self, actor_type: str = None,
                    offset: int = 0, limit: int = 50) -> dict:
        params = {"offset": offset, "limit": limit}
        if actor_type:
            params["actor_type"] = actor_type
        return self._get("/actors", **params)

    def iter_actors(self, actor_type: str = None) -> Iterator[dict]:
        offset, limit = 0, 50
        while True:
            page = self.list_actors(actor_type, offset, limit)
            yield from page["data"]
            if offset + limit >= page["pagination"]["total"]:
                break
            offset += limit

    def get_actor(self, actor_id: int, fields: list[str] = None) -> dict:
        params = {}
        if fields:
            params["fields"] = ",".join(fields)
        return self._get(f"/actors/{actor_id}", **params)

    def kill_actor(self, actor_id: int, force: bool = True) -> dict:
        return self._delete(f"/actors/{actor_id}", force=str(force).lower())

    def get_mailbox(self, actor_id: int) -> dict:
        return self._get(f"/actors/{actor_id}/mailbox")

    def get_children(self, actor_id: int) -> dict:
        return self._get(f"/actors/{actor_id}/children")

    def get_circuit_breaker(self, actor_id: int) -> dict:
        return self._get(f"/actors/{actor_id}/circuit-breaker")

    def quarantine(self, actor_id: int, reason: str) -> dict:
        return self._post(f"/actors/{actor_id}/quarantine", {"reason": reason})

    def unquarantine(self, actor_id: int) -> dict:
        return self._delete(f"/actors/{actor_id}/quarantine")

    # ── System ─────────────────────────────────────────────────

    def get_system(self) -> dict:
        return self._get("/system")

    def get_stats(self) -> dict:
        return self._get("/system/stats")

    def get_memory(self, actor_id: int = None) -> dict:
        params = {}
        if actor_id:
            params["actor_id"] = actor_id
        return self._get("/system/memory", **params)

    def drain(self) -> dict:
        return self._post("/system/drain")

    def shutdown(self) -> dict:
        return self._post("/system/shutdown")

    # ── Faults ─────────────────────────────────────────────────

    def get_faults(self) -> dict:
        return self._get("/faults")

    def clear_faults(self) -> dict:
        return self._post("/faults/clear")

    # ── DLQ ────────────────────────────────────────────────────

    def list_dlq(self, actor_id: int = None,
                 offset: int = 0, limit: int = 50) -> dict:
        params = {"offset": offset, "limit": limit}
        if actor_id:
            params["actor_id"] = actor_id
        return self._get("/dead-letter-queue", **params)

    def get_dlq_record(self, index: int) -> dict:
        return self._get(f"/dead-letter-queue/{index}")

    def replay_dlq(self, index: int, target_actor_id: int = None) -> dict:
        body = {}
        if target_actor_id is not None:
            body["target_actor_id"] = target_actor_id
        return self._post(f"/dead-letter-queue/{index}/replay", body)

    def export_dlq(self, actor_id: int = None) -> dict:
        params = {}
        if actor_id:
            params["actor_id"] = actor_id
        return self._get("/dead-letter-queue/export", **params)

    # ── Asks ───────────────────────────────────────────────────

    def list_asks(self, offset: int = 0, limit: int = 50) -> dict:
        return self._get("/asks", offset=offset, limit=limit)

    def cancel_ask(self, message_id: int) -> dict:
        return self._post(f"/asks/{message_id}/cancel")


# ── Example usage ──────────────────────────────────────────────

if __name__ == "__main__":
    client = HPActorClient()

    # List all actors
    for actor in client.iter_actors():
        print(f"Actor {actor['actor_id']}: {actor['actor_type']} [{actor['state']}]")

    # Inspect an actor's mailbox
    mbox = client.get_mailbox(42)
    print(f"Mailbox depth: {mbox['data']['depth']}/{mbox['data']['capacity']}")

    # Check for dead letters
    dlq = client.list_dlq()
    print(f"Dead letters: {dlq['pagination']['total']}")
```

### Go

```go
package hpactor

import (
    "bytes"
    "encoding/json"
    "fmt"
    "net/http"
    "strconv"
)

const DefaultBaseURL = "http://127.0.0.1:9090/api/v1"

type Client struct {
    BaseURL    string
    HTTPClient *http.Client
}

func NewClient(baseURL string) *Client {
    if baseURL == "" {
        baseURL = DefaultBaseURL
    }
    return &Client{BaseURL: baseURL, HTTPClient: &http.Client{}}
}

type ErrorResponse struct {
    Error struct {
        Code    string `json:"code"`
        Message string `json:"message"`
    } `json:"error"`
}

func (c *Client) get(path string) (*http.Response, error) {
    return c.HTTPClient.Get(c.BaseURL + path)
}

func (c *Client) post(path string, body interface{}) (*http.Response, error) {
    b, _ := json.Marshal(body)
    return c.HTTPClient.Post(
        c.BaseURL+path,
        "application/json",
        bytes.NewReader(b),
    )
}

func (c *Client) delete(path string) (*http.Response, error) {
    req, _ := http.NewRequest("DELETE", c.BaseURL+path, nil)
    return c.HTTPClient.Do(req)
}

// ListActors returns a paginated list of actors.
func (c *Client) ListActors(actorType string, offset, limit int) (*http.Response, error) {
    path := fmt.Sprintf("/actors?offset=%d&limit=%d", offset, limit)
    if actorType != "" {
        path += "&actor_type=" + actorType
    }
    return c.get(path)
}

// GetActor returns an actor's full detail.
func (c *Client) GetActor(id uint64, fields []string) (*http.Response, error) {
    path := fmt.Sprintf("/actors/%d", id)
    if len(fields) > 0 {
        path += "?fields=" + joinFields(fields)
    }
    return c.get(path)
}

// KillActor terminates an actor.
func (c *Client) KillActor(id uint64, force bool) (*http.Response, error) {
    return c.delete(fmt.Sprintf("/actors/%d?force=%s", id, strconv.FormatBool(force)))
}

func joinFields(fields []string) string {
    s := ""
    for i, f := range fields {
        if i > 0 {
            s += ","
        }
        s += f
    }
    return s
}
```

## Configuration

The HTTP API server is configured via TOML:

```toml
[system.cli.http]
port = 9090                    # Listen port
bind_address = "127.0.0.1"     # Bind address
max_connections = 100           # Max concurrent connections
legacy_cli_endpoint = true      # Enable POST /cli backward compat
```

## Known Limitations (v1)

| Limitation | Workaround |
|------------|------------|
| Circuit breaker reset returns 501 | No workaround — restart the actor |
| Per-actor memory returns 501 | Use `GET /system/memory` for system-wide stats |
| Ask enumeration returns 501 | Use `POST /cli` legacy endpoint or the interactive CLI |
| `drain()` delegates to full `shutdown()` | Use `POST /system/shutdown` directly |
| `uptime_ms` always 0 | Not tracked — monitor externally |
| No authentication | Bind to `127.0.0.1` only; do not expose to untrusted networks |

## Related Documents

- [OpenAPI 3.0 Specification](cli-http-rest-api-openapi.yaml)
- [Architecture Design Doc](cli-http-rest-api-design.md)
- [Detailed Implementation Spec](../../superpowers/specs/2026-06-16-cli-http-rest-api-detailed-design.md)
- [Implementation Plan](../../superpowers/plans/2026-06-16-cli-http-rest-api-impl.md)
