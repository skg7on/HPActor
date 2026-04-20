# RPC Channel Design Specification

**Date:** 2026-04-20
**Status:** Draft
**Author:** HPActor Team

---

## Overview

Add an asynchronous RPC (Remote Procedure Call) layer on top of the existing `TcpTransport`/`ConnectionPool` infrastructure. RPC calls are fire-and-forget with automatic retry on timeout, providing at-least-once delivery semantics for request-response interactions between actors on different nodes.

## Goals

- RPC calls are independent of the actor message system (no interference with actor mailbox processing)
- Reuses existing `ConnectionPool` for network transport
- At-least-once delivery with configurable retries
- Blocking `future::get()` API for result retrieval
- Configurable timeout per call
- `ActorContext::rpc()` convenience method for actor-initiated calls

## Non-Goals

- Connectionless (UDP) RPC — uses ConnectionPool TCP connections
- Exactly-once semantics — callers must handle idempotency
- Coroutine/callback APIs — blocking `get()` only
- Typed code generation — raw message types

---

## Architecture

### Component Hierarchy

```
ActorSystem
    └── RpcChannel (1:1 with ActorSystem)
            ├── PendingCall registry (MessageId → PendingCall)
            ├── Retry state machine
            └── ConnectionPool accessor

ActorContext
    └── rpc(target, request, timeout_ms) → RpcFuture<response>

Transport (TcpTransport)
    └── ConnectionPool
            └── TlsConnection
```

### RpcChannel

`RpcChannel` owns the pending call registry and retry logic. One instance per `ActorSystem`.

```cpp
class RpcChannel {
public:
    explicit RpcChannel(Transport* transport);

    // Initiate RPC call, returns future for response type
    template<typename Request, typename Response>
    RpcFuture<Response> call(const ActorAddress& target,
                             const Request& request,
                             std::chrono::milliseconds timeout_ms);

    // Cancel all pending calls (destructor aid)
    void abort();

private:
    void on_response(MessageId msg_id, const bytes& encoded);
    void on_timeout(MessageId msg_id);

    Transport* transport_;
    std::unordered_map<MessageId, PendingCall> pending_;
    mutable std::mutex mutex_;
};
```

### PendingCall

Tracks in-flight RPC calls with their retry state.

```cpp
struct PendingCall {
    MessageId msg_id;
    ActorAddress target;
    bytes encoded_request;
    std::chrono::milliseconds timeout;
    int retry_count = 0;
    int max_retries = 5;
    std::promise<bytes> promise;
    std::chrono::steady_clock::time_point enqueued_at;
};
```

### RpcFuture

Wrapper around `std::future` that enforces timeout on `get()`.

```cpp
template<typename T>
class RpcFuture {
public:
    RpcFuture(std::future<T> inner, std::chrono::milliseconds timeout);

    T get();  // throws error on timeout

private:
    std::future<T> inner_;
    std::chrono::milliseconds timeout_;
};
```

### Frame Extension

Add RPC-specific flags to the existing `Frame` structure:

```cpp
struct Frame {
    // ... existing fields ...

    // New RPC flag constants
    static constexpr uint32_t RpcRequest = 1 << 2;   // This frame is an RPC request
    static constexpr uint32_t RpcResponse = 1 << 3; // This frame is an RPC response
    static constexpr uint32_t RpcNoRetry = 1 << 4;   // Don't retry this RPC (already sent)
};
```

---

## Data Flow

### RPC Call Initiation (ActorContext::rpc)

1. Actor calls `context()->rpc(target, request, timeout_ms)`
2. `RpcChannel::call()` generates `MessageId`
3. Request is serialized with `TypeTag`
4. `PendingCall` created with promise, added to registry
5. `Frame` created with `RpcRequest` flag, encoded
6. `ConnectionPool::send(receiver, encoded)` transmits frame
7. `RpcFuture` returned to caller

### Response Reception

1. `TlsConnection` receives `Frame` via existing handler
2. If `Frame::RpcResponse` flag set, extract `MessageId`
3. Look up `PendingCall` by `MessageId`
4. Deserialize response into `PendingCall::promise`
5. `RpcFuture::get()` returns deserialized response

### Timeout and Retry

1. `RpcChannel` tracks `PendingCall::enqueued_at`
2. On timeout expiration:
   - If `retry_count < max_retries`: increment retry, resend with `RpcNoRetry` flag, reset timer
   - If `retry_count >= max_retries`: reject promise with `error::timeout`
3. On connection error: same retry logic

---

## ActorContext Integration

Add `rpc()` method to `ActorContext`:

```cpp
// In ActorContext (existing class)
template<typename Request, typename Response>
RpcFuture<Response> rpc(const ActorAddress& target,
                          const Request& request,
                          std::chrono::milliseconds timeout_ms = 5000);
```

Implementation delegates to `ActorSystem::rpc_channel()`.

---

## Thread Safety

- `RpcChannel::pending_` protected by `mutex_`
- `PendingCall::promise` accessed only from transport thread (response handler) and caller thread (get)
- Race: promise set after timeout checks — use atomic `ready_` flag per `PendingCall`

---

## Error Handling

| Error | Condition | Return |
|-------|-----------|--------|
| `errors::timeout` | No response after `max_retries` | `result<Response>::make(error(timeout, ...))` |
| `errors::connection_failed` | Cannot connect to target node | `result<Response>::make(error(connection_failed, ...))` |
| `errors::actor_not_found` | Target actor does not exist | From response deserialization |

---

## File Structure

```
include/hpactor/
    └── rpc/
            ├── rpc_channel.hpp      // RpcChannel, RpcFuture, PendingCall
            └── rpc.hpp              // ActorContext::rpc() integration

src/
    └── rpc/
            └── rpc_channel.cpp     // RpcChannel implementation

tests/
    └── rpc/
            └── test_rpc_channel.cpp // Unit tests
```

---

## Test Plan

1. **Unit tests**
   - `test_rpc_channel_timeout` — verify timeout error after retries
   - `test_rpc_channel_response` — verify successful response deserialization
   - `test_rpc_channel_retry` — verify retry with same MessageId
   - `test_rpc_channel_concurrent` — multiple concurrent calls
   - `test_rpc_channel_no_interference` — actor messages not blocked by pending RPC

2. **Integration test** (Phase 7)
   - Two-process test: actor on node A calls RPC to actor on node B

---

## Dependencies

- Existing `ConnectionPool` for transport
- Existing `Frame` encoding/decoding
- Existing `ActorContext` for convenience method
- `std::future` / `std::promise` for async result
- `std::chrono` for timeout/retry timing

---

## Open Issues

1. Should `RpcChannel` register with the `EventLoop` for timer callbacks, or use a background thread for timeout tracking?
2. How to handle RPC calls initiated from non-actor threads (e.g., main thread)? Provide `ActorSystem::rpc_channel()` access directly.
