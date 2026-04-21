# TlsConnection EventLoop Integration Design

**Date:** 2026-04-21
**Status:** Draft

## Overview

Refactor TlsConnection to properly integrate with EventLoop's completion callback mechanism, replacing blocking `::send()` and `::recv()` with non-blocking async I/O.

## Problem

TlsConnection currently uses blocking `::send()` in `send_raw()`, which blocks the calling thread when the kernel send buffer is full. This causes deadlocks in the event-driven architecture.

The TlsConnection has a `handle_send_completion()` method that exists but is never called - completions are not routed back to the connection because `ActorId(0)` is passed to `async_send()`.

## Architecture

```
TcpTransport
    ├── owns EventLoop (loop_)
    ├── maintains map: fd → TlsConnectionPtr
    └── sets completion_callback on EventLoop

EventLoop
    ├── backend_->async_send() → kernel
    └── completion_callback ← routes OpCompletion to TcpTransport

TlsConnection
    ├── owns TlsContext, stores loop_ reference
    ├── calls loop_->backend()->async_send() directly
    ├── registers fd via loop_->add_fd()
    └── handle_read() called by TcpTransport when data arrives
```

## Send Flow

1. `TlsConnection::send()` → `send_raw()` → `write_buffer_.insert()` → `flush_write_buffer()`
2. `flush_write_buffer()` → `loop_->backend()->async_send(fd, ...)`
3. Backend sends data asynchronously
4. On completion, backend calls `deliver_completion()` → EventLoop → completion_callback
5. TcpTransport's callback receives `OpCompletion`, looks up `TlsConnectionPtr` by fd, calls `handle_send_completion(result)`
6. `handle_send_completion()` updates state, flushes remaining `write_buffer_` if any

## Receive Flow

1. EventLoop notifies TcpTransport (via `wait()` return) that fd is readable
2. TcpTransport reads data from fd
3. TcpTransport calls `tlsConnection->handle_read(data)`
4. `handle_read()` processes TLS messages and handshake state machine

## Component Changes

### TlsConnection

**New method:**
```cpp
void set_send_completion_handler(std::function<void(int result)> handler);
```

**Changes:**
- `handle_send_completion(int result)` - made public, called by TcpTransport
- Remove `is_sending_` member (tracked by TcpTransport via completion callback)
- `write_buffer_` and `flush_write_buffer()` remain for queuing data until async_send succeeds
- Add `TcpTransport* transport_` member for error reporting

**Existing methods:**
- `send_raw(const bytes& data)` - unchanged (calls async_send, completion routes via EventLoop)

### TcpTransport

**New members:**
```cpp
std::unordered_map<int, TlsConnectionPtr> connections_;
std::function<void(OpCompletion)> completion_callback_;
```

**New methods:**
```cpp
void register_connection(TlsConnectionPtr conn);
void unregister_connection(int fd);
```

**Changes:**
- Constructor sets `completion_callback_` and calls `loop_->set_completion_callback()`
- Completion callback handler:
  ```cpp
  if (completion.type == OpType::Send) {
      auto it = connections_.find(completion.fd);
      if (it != connections_.end()) {
          it->second->handle_send_completion(completion.result);
      }
  }
  ```

## Error Handling

- **Send errors:** `handle_send_completion()` receives negative result, invokes `error_handler_`
- **Connection closes:** TcpTransport removes from `connections_` map, calls `TlsConnection::close()`
- **TLS handshake errors:** `TlsConnection` invokes `error_handler_` directly

## Implementation Steps

1. Add `set_send_completion_handler()` to TlsConnection
2. Add `connections_` map and `register_connection()`/`unregister_connection()` to TcpTransport
3. Set completion callback in TcpTransport constructor
4. Route send completions in callback handler
5. Remove `is_sending_` from TlsConnection (no longer needed)
6. Verify all tests pass

## Testing

- Unit test: TlsConnection send with mock EventLoop
- Integration test: TcpTransport + EventLoop + TlsConnection end-to-end with socketpair
- Verify send completions route to correct TlsConnection instance
- Verify error handling invokes error_handler_
