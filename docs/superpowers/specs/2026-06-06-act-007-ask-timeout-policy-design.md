# ACT-007: Standardized Ask/Request Timeout Policy — Design Spec

**Issue:** [#12](https://github.com/skg7on/HPActor/issues/12)
**Date:** 2026-06-06
**Status:** Draft
**Subsystem:** Actor Runtime, RPC, Spawn

## 1. Problem Statement

Four request-response flows exist in HPActor, each with different timeout behavior
(or none at all). There is no unified timeout policy, no consistent API for the
caller to specify a deadline, and no guarantee that a caller waiting on a reply
will ever be unblocked.

| Flow | Has timeout? | Per-call override? | Retry? | Caller API |
|------|:---:|:---:|:---:|------|
| Local actor ask | No | N/A | No | Manual `send()`+`reply()` |
| Remote actor ask | No | N/A | No | Manual `send()`+`reply()` via ActorProxy |
| RPC (`context()->rpc()`) | Yes (5s) | Yes (`timeout_ms` param) | Up to 5 | Returns `RpcFuture`, blocks on `.get()` |
| Remote spawn (`spawn_remote_async()`) | Yes (5s global) | No | No | Returns `AsyncActor`, blocks on `.get()` |

Additional issues uncovered:

- **RPC retry has no backoff and no total-deadline concept.** `PendingCall::enqueued_at` is stored but never consulted. A call with `timeout=1s` and 5 retries takes up to 6s wall-clock.
- **Spawn response handler is not wired.** `ConnectionPool::set_spawn_handler()` exists and decodes `SpawnResponseTag` frames, but no code calls `set_spawn_handler()`. Spawn responses fall through to `RpcChannel::on_response()`, which has no entry for the message ID, so responses are silently dropped. `pending_spawns_` is populated but never consumed.
- **No deadline/time-budget concept.** Message-level deadlines exist in the mailbox subsystem (`deadline_ns` on `MailboxEnvelopeMeta`) and `FailureReason::Expired` exists, but no request-response flow uses them.
- **No failure envelope on timeout.** `RpcChannel::on_timeout()` resolves the future with `error(errors::timeout)` but does not emit a `FailureEnvelope`. Spawn timeout returns `error(errors::timeout)` only to the caller.

## 2. Design Goals

1. **Unified timeout policy** — every request-response flow accepts an optional
   per-call timeout with a sensible global default.

2. **Deadline, not just timeout** — the caller specifies an absolute deadline or
   a time budget; the system tracks elapsed wall-clock time across retries so
   total wait time is bounded.

3. **Consistent caller API** — local ask, remote ask, RPC, and spawn all return
   a future-like handle that the caller can wait on, cancel, or check readiness.

4. **Observable failures** — timeouts produce `FailureEnvelope` entries with
   full correlation metadata, metrics events, and structured log warnings.

5. **Wire the spawn response path** — close the gap so remote spawn responses
   actually reach the `AsyncActor` handle.

6. **Preserve source compatibility** — existing `context()->rpc()` and
   `spawn_remote_async()` call sites remain valid; new richer APIs are additive.

## 3. Unified Timeout Model

### 3.1 Core Type: `RequestTimeout`

```cpp
/// \brief Per-request timeout specification.
///
/// Represents either a relative duration or an absolute deadline.
/// A zero-value means "use the system default."
struct RequestTimeout {
    enum class Kind { Duration, Deadline };

    Kind kind = Kind::Duration;
    std::chrono::milliseconds value{0};

    static RequestTimeout from_ms(uint64_t ms);
    static RequestTimeout from_deadline(std::chrono::steady_clock::time_point tp);
    static RequestTimeout use_default();

    /// Compute the point in time when this request expires.
    std::chrono::steady_clock::time_point deadline() const;

    /// Whether this is using the system default (value == 0ms).
    bool is_default() const { return value.count() == 0; }
};
```

### 3.2 System Default Configuration

Two new fields in the X-macro system config table (`system_fields.def`):

```
HPACTOR_SYSTEM_FIELD(default_ask_timeout_ms, std::chrono::milliseconds, "ask.default_timeout_ms", std::chrono::milliseconds{5000})
HPACTOR_SYSTEM_FIELD(default_ask_max_retries, uint32_t, "ask.max_retries", uint32_t{3})
```

TOML config:

```toml
[system.ask]
default_timeout_ms = 5000   # per-attempt timeout for ask/rpc/spawn
max_retries = 3             # default max retries (0 = no retry)
```

The existing `spawn_timeout_ms` field remains but is deprecated — if
`default_ask_timeout_ms` is non-zero it takes precedence for spawn flows.

### 3.3 Per-Flow Defaults (Compile-Time Sensible Defaults)

| Flow | Default timeout | Default retries | Reason |
|------|:---:|:---:|------|
| Local ask | 5s | 0 | Same process, no network; retry pointless |
| Remote ask | 5s | 2 | Network possible, moderate retries |
| RPC | 5s | 3 | Explicit RPC, more tolerant |
| Spawn | 10s | 3 | Actor creation is expensive; longer budget |

These are **compile-time defaults** overridable per-call and per-system via TOML.

## 4. Unified Caller API

### 4.1 `RequestHandle<T>` — replaces `RpcFuture<T>` and `AsyncActor`

```cpp
/// \brief Handle to a pending request with optional timeout.
///
/// Move-only. The caller can:
/// - Block on get() with the configured timeout.
/// - Check ready() without blocking.
/// - Cancel the pending request.
///
/// \tparam T The result type (e.g., StreamBuffer, ActorRef).
template <typename T>
class RequestHandle {
public:
    RequestHandle();
    RequestHandle(RequestHandle&&) noexcept;
    RequestHandle& operator=(RequestHandle&&) noexcept;
    ~RequestHandle();

    /// Block until response arrives or the request times out.
    /// Returns the response or an error (timeout, cancelled, etc.).
    result<T> get();

    /// Non-blocking readiness check.
    bool ready() const;

    /// Cancel the pending request. Any blocked get() returns errors::cancelled.
    void cancel();

    /// The message_id used for this request (for tracing correlation).
    MessageId message_id() const;

    /// The deadline after which this request is considered expired.
    std::chrono::steady_clock::time_point deadline() const;

private:
    // internal: std::promise + mutex + cv, or a lock-free state machine
    // friend: AskManager, RpcChannel, ActorSystem
};
```

`RpcFuture<StreamBuffer>` is re-aliased to `RequestHandle<StreamBuffer>`.
`AsyncActor` is replaced by `RequestHandle<ActorRef>` (with a type alias
for backward compatibility).

### 4.2 New `context()->ask()` Methods

```cpp
// ActorContext — for use from within an actor

/// Send a request to a local actor and get a handle for the response.
/// The response is delivered to this actor's mailbox as normal,
/// but the handle is resolved when the matching reply arrives.
template <typename ReqT, typename ResT>
RequestHandle<ResT> ask(
    const ActorAddress& target,
    const ReqT& request,
    RequestTimeout timeout = RequestTimeout::use_default());

/// Send a request to a remote actor (via ActorProxy) with a timeout.
/// Internally uses RPC semantics but the caller uses the same API.
template <typename ReqT, typename ResT>
RequestHandle<ResT> ask(
    const ActorAddress& target,
    const ReqT& request,
    RequestTimeout timeout = RequestTimeout::use_default());

/// Low-level raw ask (for pre-encoded buffers).
RequestHandle<StreamBuffer> ask_raw(
    const ActorAddress& target,
    const StreamBuffer& encoded_request,
    RequestTimeout timeout = RequestTimeout::use_default());
```

The existing `context()->rpc(target, request, timeout_ms)` is retained as a
convenience overload that delegates to `ask_raw()`.

### 4.3 Unified `spawn_remote_async()`

```cpp
// ActorSystem

/// Async remote spawn with optional per-call timeout.
RequestHandle<ActorRef> spawn_remote_async(
    const std::string& node_name,
    const std::string& actor_type,
    const StreamBuffer& args,
    RequestTimeout timeout = RequestTimeout::use_default());

/// Synchronous convenience wrapper.
result<ActorRef> spawn_remote(
    const std::string& node_name,
    const std::string& actor_type,
    const StreamBuffer& args,
    RequestTimeout timeout = RequestTimeout::use_default());
```

## 5. Deadline vs. Timeout Semantics

### 5.1 Per-Attempt Timeout

The `timeout` value in `RequestTimeout` (when `kind == Duration`) is the
**per-attempt** timeout — how long to wait for a response to a single send.

### 5.2 Overall Deadline

The `RequestTimeout` also supports `kind == Deadline` — an absolute wall-clock
time after which the request is considered failed regardless of retry state.

When both a per-attempt timeout and an overall deadline are needed, the caller
passes a `Deadline`. The implementation computes `min(per_attempt_timeout,
remaining_until_deadline)` before each send.

### 5.3 Retry with Deadline

```
rc = 0
deadline = now + timeout  (or caller-provided absolute deadline)
loop:
    remaining = deadline - now
    if remaining <= 0: fail with Expired
    send_request()
    wait_for_response(min(per_attempt_timeout, remaining))
    if response_ok: return response
    if rc++ >= max_retries: fail with Timeout
    // optional: backoff
```

### 5.4 Backoff (Future Enhancement)

Not in v1 scope. The current implementation uses fixed-interval retry (same
timeout per attempt). A future `RetryPolicy` struct could add exponential
backoff with jitter:

```cpp
struct RetryPolicy {
    uint32_t max_retries = 3;
    std::chrono::milliseconds initial_backoff{100};
    float backoff_multiplier = 2.0f;
    std::chrono::milliseconds max_backoff{5000};
    bool jitter = true;
};
```

This is noted here for API compatibility planning but is **not part of the
current implementation scope**.

## 6. Local ask() Implementation

### 6.1 How It Works

When `actor A` calls `ask<Req, Res>(target_B, req, timeout)`:

1. **AskManager** (new subsystem, owned by ActorSystem) generates a `MessageId`
   and stores a `PendingAsk` record keyed by `(actor_A_id, message_id)`.

2. A `TypedMessage` is sent to `target_B` with:
   - The request payload and TypeTag.
   - `sender_address` set to `A`'s address (so `B` can `reply()`).
   - A new `ask_message_id` field in `TypedMessage` metadata identifying this
     as a tracked ask.

3. If a timeout was specified, a timer is scheduled via the TimingWheel. When
   it fires, `AskManager::on_timeout(msg_id)` resolves the handle with
   `error(errors::timeout)` and records a `FailureEnvelope`.

4. When `B` calls `context()->reply(response)`, `ActorContext::reply()` checks
   whether the `current_sender_` message had an `ask_message_id`. If so, it
   routes the response through `AskManager::on_response(msg_id, response)`,
   which resolves the handle.

5. **AskManager** hooks into `EventBasedActor::receive()` — when an incoming
   message carries a reply to a tracked ask, `receive()` short-circuits to
   resolve the handle rather than queuing the message in the actor's mailbox.

### 6.2 Race: Response arrives before ask() registers

```cpp
AskManager::register_ask(actor_id, msg_id, handle, timeout);
send(target, msg);  // <= response could arrive here, before we register
```

Fixed by registering the handle **before** sending the message. The response
handler checks `pending_asks_` atomically.

### 6.3 Integration with existing on_request

No changes to `on_request` or `on_proto_message`. The `reply()` path is
transparent — it already calls `context()->reply()`. The only change is in
`ActorContext::reply()` to check for `ask_message_id` and route through
`AskManager` when present.

## 7. Remote ask() Implementation

### 7.1 Reusing RpcChannel

Remote `ask()` reuses the existing `RpcChannel` mechanism:

1. `ActorContext::ask(target, req, timeout)` detects that `target` is remote
   (via `ActorRef::is_local()`).

2. Serializes the request via `DefaultSerializer`, then calls
   `RpcChannel::call_raw()` with the timeout.

3. `RpcChannel` already handles timeout, retry, and correlation by `message_id`.

4. The result is wrapped in `RequestHandle<StreamBuffer>`. The caller
   deserializes the response.

### 7.2 Typed Remote ask

A typed wrapper deserializes automatically:

```cpp
template <typename ReqT, typename ResT>
RequestHandle<ResT> ActorContext::ask(const ActorAddress& target,
                                       const ReqT& request,
                                       RequestTimeout timeout) {
    auto encoded = serialize_request(request);
    auto raw_handle = ask_raw(target, encoded, timeout);
    // Return a wrapper that deserializes ResT on get()
    return RequestHandle<ResT>::from_raw(std::move(raw_handle));
}
```

## 8. RPC Channel Hardening

Changes to `src/rpc/rpc_channel.cpp`:

### 8.1 Use `enqueued_at` for deadline enforcement

```cpp
// In on_timeout():
auto elapsed = steady_clock::now() - call->enqueued_at;
auto total_budget = call->deadline - call->enqueued_at;
if (elapsed >= total_budget) {
    // Total deadline exceeded — fail permanently
    resolve_with_error(call, errors::expired, "RPC deadline expired");
    return;
}
```

### 8.2 `max_retries` from config, not hardcoded

```cpp
// PendingCall construction:
call->max_retries = channel_config_.default_ask_max_retries;
```

### 8.3 Emit FailureEnvelope on timeout/expiry

```cpp
// In on_timeout() when retries exhausted:
auto envelope = make_failure_envelope(
    FailureReason::Timeout,
    FailureSource::Rpc,
    call->target,
    /* sender = */ local_actor_id_,
    /* message_id = */ call->msg_id,
    /* trace = */ call->trace,
    /* retryable = */ true,
    /* detail = */ "RPC call timed out after N retries"
);
system_->emit_failure_envelope(envelope);
```

### 8.4 Wire fault injection for deadline expiry

Add a fault point `"hpactor.rpc.deadline.drop"` in `on_timeout` that simulates
a missed deadline check.

## 9. Spawn Flow Wiring Fix

### 9.1 The Gap

`ConnectionPool::set_spawn_handler()` is defined but **never called**. Spawn
responses arrive with `RpcResponse` flag and `SpawnResponseTag` type tag, but
since `spawn_handler_` is null, they fall through to `rpc_handler_` (i.e.,
`RpcChannel::on_response()`), which has no matching `message_id` in its
`pending_` map, so the response is silently dropped.

### 9.2 Fix: Route spawn responses to ActorSystem

```
Option A (minimal): Have TcpTransport/ActorSystem call
  connection_pool->set_spawn_handler() during init,
  pointing to ActorSystem::on_spawn_response().

Option B (unified): Fold spawn into RpcChannel — register spawn
  calls as PendingCall entries in RpcChannel::pending_ so
  on_response() resolves them naturally.

Recommendation: Option B. Spawn is just another request-response
pattern. The only difference is the caller gets ActorRef instead
of StreamBuffer.
```

### 9.3 Option B Implementation

1. `ActorSystem::spawn_remote_async()` serializes the `SpawnRequestMessage`
   and calls `rpc_channel_->call_raw()` with the serialized payload, `TypeTag::SpawnRequestTag`, and the spawn timeout.

2. `RpcChannel::call_raw()` gains an overload that accepts a
   `std::function<void(result<StreamBuffer>)>` completion callback, or
   alternatively, `PendingCall` gains a `result_type` discriminator so
   `on_response()` can route to the correct handler.

3. When the `SpawnResponse` arrives at `ConnectionPool::on_frame_received()`,
   it has `RpcResponse` flag set, so it goes to `rpc_handler_`.
   `RpcChannel::on_response()` finds the `PendingCall`, resolves the promise,
   and the `RequestHandle<ActorRef>` decodes the `SpawnResponse` protobuf.

4. The `pending_spawns_` map in `ActorSystem` is **removed** — spawn state
   lives in `RpcChannel::pending_` instead.

### 9.4 Backward Compatibility

Existing `spawn_remote_async()` signature is preserved. The internal
implementation changes to use `RpcChannel` instead of direct transport send +
`pending_spawns_`.

## 10. Observability

### 10.1 Metrics

New metric event types (add to `MetricEventType` enum):

| Event | Description |
|-------|-------------|
| `kAskSent` | An ask was sent (local or remote) |
| `kAskCompleted` | An ask completed successfully |
| `kAskTimeout` | An ask timed out (per-attempt) |
| `kAskExpired` | An ask's overall deadline expired |
| `kAskRetry` | An ask was retried |
| `kAskCancelled` | An ask was cancelled by the caller |

Existing `kRpcCallSent`, `kRpcResponseReceived`, `kRpcTimeout` events
are subsumed by these (keep the old events as aliases for backward compat).

### 10.2 Tracing

Each ask creates a **client span** (`SpanKind::kClient`) with attributes:

- `ask.target` — target ActorId or node+actor
- `ask.message_id` — correlation ID
- `ask.timeout_ms` — configured timeout
- `ask.deadline` — absolute deadline timestamp

The span ends when the response arrives or the ask times out.

### 10.3 Logging

Structured log events on timeout/expiry:

```
[WARN] Ask timed out: target=<actor_id>, msg_id=<id>, attempt=2/3,
       elapsed_ms=4500, timeout_ms=5000, deadline_ms=15000
```

### 10.4 CLI

New CLI commands:

```
/ask pending              List all pending asks (actor_id, target, age, deadline)
/ask pending <actor_id>   Filter by actor
/ask cancel <msg_id>      Cancel a specific pending ask
/ask stats                Aggregate ask metrics (sent, completed, timeout, expired)
```

### 10.5 Dead-Letter Queue

When an ask times out or expires, the request message is recorded in the DLQ
with `DeadLetterReason::AskTimeout` (new reason). The DLQ record includes the
original request payload, target, and deadline for replay.

## 11. Testing Strategy

### 11.1 Unit Tests

| Test | What it validates |
|------|-------------------|
| `test_request_timeout` | `RequestTimeout` construction, deadline computation, default behavior |
| `test_ask_manager_local` | `AskManager` register, response correlation, timeout, cancel |
| `test_ask_manager_race` | Response arriving before/during registration |
| `test_ask_manager_deadline` | Per-attempt timeout vs. overall deadline distinction |
| `test_ask_local_send_reply` | End-to-end local ask: send request, handler replies, handle resolves |
| `test_ask_local_timeout` | Local ask times out, handle returns error, FailureEnvelope emitted |
| `test_ask_local_cancel` | Cancel before response, handle returns cancelled |
| `test_ask_remote` | Remote ask via RpcChannel, successful response |
| `test_ask_remote_timeout` | Remote ask timeout with retry exhaustion |
| `test_ask_remote_retry` | Remote ask retries, eventual success |
| `test_ask_deadline_vs_timeout` | Deadline expires before retries exhausted |
| `test_spawn_wired` | Spawn response reaches the handle (validates wiring fix) |
| `test_spawn_timeout_override` | Per-call spawn timeout overrides system default |
| `test_failure_envelope_on_ask_timeout` | FailureEnvelope emitted with correct metadata |
| `test_rpc_uses_enqueued_at` | RPC respects total deadline across retries |
| `test_rpc_max_retries_from_config` | max_retries comes from config, not hardcoded |

### 11.2 Integration Tests

| Test | What it validates |
|------|-------------------|
| `test_ask_actor_to_actor_local` | Two local actors, ask pattern |
| `test_ask_actor_to_actor_remote` | Local actor asks remote actor |
| `test_spawn_remote_end_to_end` | Full remote spawn: request, create, response, handle resolution |
| `test_ask_under_backpressure` | Ask when target mailbox is full — rejected with backpressure signal |
| `test_ask_tracing` | Trace context propagated through ask flow |
| `test_ask_metrics` | Metric events emitted for ask lifecycle |

### 11.3 System Tests

| Test | What it validates |
|------|-------------------|
| `test_ask_cli_commands` | CLI /ask pending, /ask cancel, /ask stats |
| `test_ask_dlq_integration` | Expired asks recorded in DLQ, replayable |
| `test_ask_shutdown` | Pending asks resolved during graceful shutdown |
| `test_ask_fault_injection` | Fault points trigger expected timeout/deadline behavior |

### 11.4 Regression Tests

Existing tests that must continue to pass:

- `test_rpc_channel` (all 15 tests) — existing RPC behavior preserved
- `test_async_actor` — `AsyncActor` renamed/aliased, same contract
- `test_spawn_integration` — spawn flow works end-to-end after wiring fix
- `test_actor_context_rpc` — `context()->rpc()` still works

## 12. Implementation Phases

### Phase 1: Foundation Types (no behavior changes)

1. Add `RequestTimeout` struct to `include/hpactor/types/`.
2. Add `default_ask_timeout_ms` and `default_ask_max_retries` to system_fields.def.
3. Add TOML `[system.ask]` parser (`src/config/parsers/ask_config_parser.cpp`).
4. Define `RequestHandle<T>` replacing `RpcFuture<T>`, alias `AsyncActor` to it.
5. Add new metric event types and `DeadLetterReason::AskTimeout`.
6. Add CLI command stubs.

### Phase 2: RPC Hardening

1. Honor `enqueued_at` for deadline enforcement in `RpcChannel::on_timeout()`.
2. Read `max_retries` from config, not hardcoded.
3. Emit `FailureEnvelope` on timeout/expiry.
4. Add fault injection points for deadline paths.
5. Update `test_rpc_channel` for new behavior, add deadline tests.

### Phase 3: AskManager + Local ask()

1. Implement `AskManager` subsystem (`include/hpactor/actor/ask_manager.hpp`,
   `src/actor/ask_manager.cpp`).
2. Wire `AskManager` into `ActorSystem` (owned, lifetime).
3. Add `ask_message_id` to `TypedMessage` metadata.
4. Implement `ActorContext::ask()` for local targets.
5. Modify `ActorContext::reply()` to route through `AskManager` when
   `ask_message_id` is present.
6. Modify `EventBasedActor::receive()` for ask response short-circuit.
7. Write `test_ask_manager_local`, `test_ask_local_send_reply`, etc.

### Phase 4: Remote ask() + Spawn Wiring

1. Implement `ActorContext::ask()` for remote targets (delegates to RpcChannel).
2. Fold spawn into RpcChannel — `spawn_remote_async()` uses `rpc_channel_->call_raw()`.
3. Remove `pending_spawns_` from `ActorSystem`.
4. Wire `SpawnResponse` routing through `RpcChannel::on_response()`.
5. Update `spawn_remote_async()` to accept `RequestTimeout` parameter.
6. Write spawn wiring tests and remote ask tests.

### Phase 5: Observability + CLI

1. Add ask metric events to `Aggregator` and `MetricsActor`.
2. Add ask spans to `TraceManager`.
3. Implement CLI `/ask` commands in `src/cli/commands/ask_commands.cpp`.
4. Add `DeadLetterReason::AskTimeout` to DLQ overflow handler integration.
5. Write CLI, metrics, tracing, and DLQ integration tests.

### Phase 6: Cleanup + Regression

1. Deprecate `spawn_timeout_ms` (keep for backward compat).
2. Run full test suite, address regressions.
3. Update `CLAUDE_MEMORY.md` with new feature status.
4. Update `docs/superpowers/tutorials/actor-framework-tutorial.md` with ask examples.

## 13. Open Questions

1. **Backoff policy for v1?** The proposal defers exponential backoff to a
   future `RetryPolicy`. Should a simple linear backoff (e.g., `timeout * retry_count`)
   be included in v1 to prevent thundering-herd retries?

2. **Local ask() delivery model.** Should local `ask()` use mailbox delivery
   (the response goes through the caller's mailbox as a normal message) or
   direct resolution (the handle is resolved before the message enters the
   mailbox)? Direct resolution has lower latency but bypasses mailbox ordering
   guarantees. **Recommendation:** direct resolution for ask (the caller
   explicitly opted into a future, not a queued message).

3. **`on_request` timeout on the server side?** Should the server-side handler
   have a deadline after which the response is not sent? This is a separate
   concern (server-side handler SLA) and is **out of scope for this spec**.

4. **Should `spawn_timeout_ms` be removed or deprecated?** Removing it breaks
   existing TOML configs. Deprecating it (ignored if `default_ask_timeout_ms`
   is set) preserves backward compat. **Recommendation:** deprecate, remove in
   a future breaking-change release.

## 14. References

- [Production Reliability Plane](../../architecture/production/production-reliability-plane.md)
- [Architecture Requirement Backlog](../../architecture/production/architecture-requirement-backlog.md) § ACT-007
- [Structured Failure Envelope Design](../../architecture/production/structured-failure-envelope-design.md)
- [Actor Delivery Semantics Design](../../architecture/production/actor-delivery-semantics-design.md)
- [Dead-Letter Queue Design](../../architecture/production/dead-letter-queue-design.md)
- Source files analyzed: `src/rpc/rpc_channel.cpp`, `src/spawn.cpp`, `src/actor/actor_system.cpp`, `src/actor/actor_context.cpp`, `src/ref/actor_proxy.cpp`, `src/net/connection_pool.cpp`, `include/hpactor/rpc/rpc_channel.hpp`, `include/hpactor/actor_context.hpp`, `include/hpactor/spawn.hpp`, `include/hpactor/config/system_fields.def`
