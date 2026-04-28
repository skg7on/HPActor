# Unified Read Abstraction for Reactor vs Proactor

**Date:** 2026-04-28
**Owner:** SKG7ON
**Status:** Draft
**Type:** Feature Implementation

## Overview

`PlainConnection` registers its fd with the event loop via `add_fd(fd, Read)` and exposes `handle_read(const bytes&)` for framing, but the reactor backends (epoll, kqueue) never call it. Both backends implement `set_read_handler` as a no-op, and their `wait()` loops only service fds that have pending async operations (`async_recv`, `async_send`, etc.). This design bridges the gap by implementing `set_read_handler` properly in reactor backends and providing a unified API so `PlainConnection` works regardless of backend type.

---

## Problem Statement

### Current Flow (Broken)

```
PlainConnection::create_server(fd, loop)
  ├── loop->add_fd(fd, Read)           // fd registered with kqueue/epoll ✓
  └── (no read handler set)            // nobody will read from this fd ✗

Backend::wait()
  └── for each readable fd event:
        ├── if fd in pending_ops_ → process_pending_op()  // PlainConnection never creates pending ops
        ├── if fd in accept_actors_ → handle accept       // not a listening socket
        └── else → skip                                    // DATA IS NEVER READ ✗
```

### Root Cause

`PlainConnection` was designed for a callback-based model where the event loop reads data and calls `handle_read()`. But the reactor backends were built for an operation-based model where the user issues `async_recv` and gets a completion. The bridge — `set_read_handler` — exists in the interface but is a hardcoded no-op:

```cpp
// epoll_backend.hpp:80-83
void set_read_handler(int fd, read_callback handler) override {
    // No-op
}

// kqueue_backend.hpp:85-88
void set_read_handler(int fd, read_callback handler) override {
    // No-op
}
```

Similarly, `EventLoop::set_read_handler` is a no-op stub:

```cpp
// event_loop.cpp:130-132
void EventLoop::set_read_handler(int /*fd*/, read_callback /*handler*/) {
    // No-op: Reactor mode uses fd_actors_ map instead of read handlers
}
```

### Affected Code Paths

**Client-side PlainConnection** (tcp_transport.cpp:132-142, 203-211): Created via `connect()` or `connect_unix_domain()`. Has `frame_handler_` wired to `ConnectionPool::on_frame_received`. Data is never delivered.

**Server-side PlainConnection** (tcp_transport.cpp:274): Created via `handle_accept()`. Registered via `register_connection()` but has NO frame_handler or ready_handler wired at all. Even if data were read, there's no consumer.

---

## Design

### Core Principle

**Reactor backends read data synchronously in `wait()` and push it to callbacks. Proactor backends read asynchronously via `async_recv` and deliver completions.**

The caller (`PlainConnection`) uses a single `start_read(fd, callback)` API on `EventLoop`. The implementation diverges inside the backend.

### Interface Changes

#### IReactorBackend — New Query Method

```cpp
class IReactorBackend {
public:
    // ... existing methods ...

    // Returns true if this backend can call read handlers directly from wait().
    // Reactor backends (epoll, kqueue) return true.
    // Proactor backends (io_uring, GCD) return false and require async_recv.
    virtual bool supports_read_handler() const = 0;

    // These are already in the interface, currently no-ops.
    // Reactor backends will implement them. Proactor backends keep the no-op.
    virtual void set_read_handler(int fd, read_callback handler) = 0;
    virtual void clear_read_handler(int fd) = 0;
};
```

#### EventLoop — Un-stub and Expose

```cpp
class EventLoop {
public:
    // ... existing methods ...

    // Delegates to backend. No longer a no-op.
    void set_read_handler(int fd, read_callback handler);

    // Delegates to backend. No longer a no-op.
    void clear_read_handler(int fd);

    // Expose backend capability for callers that need to choose a path.
    bool supports_read_handler() const;
};
```

### Reactor Backend Implementation

#### Data Structure

```cpp
class EpollBackend : public IReactorBackend {
    // ... existing members ...

    // fd → read callback for PlainConnection-style consumers
    std::unordered_map<int, read_callback> read_handlers_;
};
```

#### set_read_handler / clear_read_handler

```cpp
void EpollBackend::set_read_handler(int fd, read_callback handler) {
    if (handler) {
        read_handlers_[fd] = std::move(handler);
    } else {
        read_handlers_.erase(fd);
    }
}

void EpollBackend::clear_read_handler(int fd) {
    read_handlers_.erase(fd);
}
```

#### wait() Dispatch Logic

In `EpollBackend::wait()`, after `process_pending_op()`, add a fallback for fds that have a read handler but no pending operation:

```
EpollBackend::wait(timeout_ms)
  │
  ├── epoll_wait() → events[]
  │
  ├── for each event:
  │     ├── if fd == timerfd_ → process_timers()
  │     └── else:
  │           ├── if fd in pending_ops_ → process_pending_op(fd, events)
  │           ├── if fd in read_handlers_ → service_read_handler(fd)    // NEW
  │           └── else → skip (no consumer)
  │
  └── return num_events
```

#### service_read_handler

```cpp
void EpollBackend::service_read_handler(int fd) {
    auto it = read_handlers_.find(fd);
    if (it == read_handlers_.end()) return;

    // Read in a loop until EAGAIN (edge-triggered safety)
    uint8_t buf[65536];  // 64KB stack buffer
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            bytes data(buf, buf + n);
            it->second(data);  // call PlainConnection::handle_read
        } else if (n == 0) {
            // EOF — connection closed
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // drained
            }
            // Error — let error_handler deal with it
            break;
        }
    }
}
```

Same pattern for `KqueueBackend::wait()`. The kqueue version reads events from `events_[]` (stored during `wait()`) and processes them similarly.

### Proactor Backend Path (Future)

Proactor backends return `supports_read_handler() == false`. When `PlainConnection` detects this, it falls back to the `async_recv` completion chain:

```
PlainConnection (proactor path)
  │
  ├── allocate read_buffer_ member (e.g., 64KB)
  ├── backend->async_recv(fd, &iov, 1, actor_id, Recv)
  │
  └── on completion via enqueue_completion:
        ├── bytes data(completion.buf, completion.result)
        ├── handle_read(data)
        └── re-issue async_recv for next read
```

This path requires `PlainConnection` to know its own `ActorId` for completion routing, which needs further design. Since proactor is OFF by default on macOS and the reactor path covers all current use cases, this is deferred to a follow-up.

### PlainConnection Changes

#### create_server

```cpp
PlainConnectionPtr PlainConnection::create_server(int fd, ..., EventLoop* loop) {
    // ... existing setup ...

    if (loop && fd >= 0) {
        loop->add_fd(fd, EventLoop::Event::Read);
        if (loop->supports_read_handler()) {
            // Reactor path: event loop reads and calls handle_read
            auto self = conn->shared_from_this();  // or weak_ptr
            loop->set_read_handler(fd, [self](const bytes& data) {
                self->handle_read(data);
            });
        }
        // Proactor path (future): issue async_recv
    }
    return conn;
}
```

#### create_client

Same pattern — after creating the PlainConnection with an already-connected fd, register the read handler.

### TcpTransport Server-Side Wiring Fix

`handle_accept` currently creates `PlainConnection` but doesn't wire frame handlers. The connection is registered but inert. Fix:

```cpp
void TcpTransport::handle_accept(int client_fd, CommunicationEndpoint remote_endpoint) {
    ConnectionPtr conn;
    if (pool_config_.use_tls) {
        conn = TlsConnection::create_server(client_fd, remote_endpoint,
                                             &tls_context_, &loop_);
    } else {
        conn = PlainConnection::create_server(client_fd, remote_endpoint, &loop_);
    }

    // Wire handlers so data flows to ConnectionPool
    auto pool = get_or_create_pool(remote_endpoint);
    conn->set_frame_handler([pool](const bytes& data) {
        pool->on_frame_received(data);
    });
    conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
        pool->on_connection_error(c, e);
    });

    pool->add_connection(conn);
    register_connection(conn, client_fd);
}
```

This ensures server-accepted PlainConnections route data through the same `ConnectionPool → TcpTransport → ActorSystem` pipeline as client-initiated ones.

---

## File Changes

### Modified Files

| File | Changes |
|------|---------|
| `include/hpactor/net/reactor_backend.hpp` | Add `supports_read_handler()` = 0 to `IReactorBackend` |
| `include/hpactor/net/reactor/epoll_backend.hpp` | Add `read_handlers_` map; override `supports_read_handler()`, `set_read_handler`, `clear_read_handler` |
| `src/net/reactor/epoll_backend.cpp` | Implement `set_read_handler`/`clear_read_handler`; add `service_read_handler()`; call from `wait()` |
| `include/hpactor/net/reactor/kqueue_backend.hpp` | Add `read_handlers_` map; override `supports_read_handler()`, `set_read_handler`, `clear_read_handler` |
| `src/net/reactor/kqueue_backend.cpp` | Implement `set_read_handler`/`clear_read_handler`; add `service_read_handler()`; call from `wait()` event loop |
| `include/hpactor/net/event_loop.hpp` | Un-stub `set_read_handler`/`clear_read_handler`; add `supports_read_handler()` |
| `src/net/event_loop.cpp` | Implement `set_read_handler`/`clear_read_handler` delegating to backend; add `supports_read_handler()` |
| `src/net/plain_connection.cpp` | Wire `set_read_handler` in `create_server` and `create_client` |
| `src/net/tcp_transport.cpp` | Wire `frame_handler`/`error_handler` on server-accepted connections in `handle_accept` |

### Proactor Backend Stubs (Minimal)

| File | Changes |
|------|---------|
| `include/hpactor/net/proactor/iouring_backend.hpp` | `supports_read_handler()` returns false |
| `include/hpactor/net/proactor/gcd_backend.hpp` | `supports_read_handler()` returns false |

---

## Data Flow After Fix

### Reactor Path (epoll/kqueue)

```
Network bytes arrive on socket fd
  │
  ▼
Backend::wait()
  │  epoll_wait/kevent returns readable event for fd
  │  fd not in pending_ops_ → check read_handlers_
  │  fd found in read_handlers_ → service_read_handler(fd)
  ▼
service_read_handler(fd)
  │  ::read(fd, buf, 64KB) in loop until EAGAIN
  │  read_handlers_[fd](data)
  ▼
PlainConnection::handle_read(data)              [plain_connection.cpp:97]
  │  Accumulate in read_buffer_
  │  4-byte length prefix framing
  │  For each complete frame: frame_handler_(frame)
  ▼
ConnectionPool::on_frame_received(frame)        [connection_pool.cpp:190]
  │  WireFrame::decode → actor_message_handler_
  ▼
TcpTransport → ActorSystem::deliver_remote → deliver_local
  │  mailbox->push(msg)
  ▼
Scheduler → EventBasedActor::receive(msg)
```

### Proactor Path (io_uring/GCD, Future)

```
PlainConnection issues async_recv(fd, bufs, ...)
  │
  ▼
Backend processes async_recv → kernel reads data
  │
  ▼
Completion arrives → enqueue_completion
  │
  ▼
PlainConnection processes completion
  │  handle_read(completion.data)
  │  issue next async_recv
  ▼
ConnectionPool::on_frame_received → ... → actor receive
```

---

## Edge Cases

### Connection Close (EOF)

`::read()` returns 0. The `service_read_handler` loop breaks. The next `wait()` cycle will either get an error event or the fd will be removed by `PlainConnection::close()` which calls `loop->remove_fd(fd)` and clears the read handler.

### Read Error

`::read()` returns -1 with errno != EAGAIN. The loop breaks. The connection error path is triggered by the next event on the fd (EPOLLERR/EPOLLHUP on epoll, EV_ERROR/EV_EOF on kqueue). The `PlainConnection` error handler calls `ConnectionPool::on_connection_error` which handles cleanup.

### Partial Frame

`PlainConnection::handle_read` already handles this — it accumulates in `read_buffer_` and only emits complete frames (4-byte length prefix). Incomplete frames stay in the buffer until more data arrives.

### Multiple Connections on Same Event Loop

Each fd gets its own entry in `read_handlers_`. The map lookup in `wait()` is O(1). Lambda captures keep the `shared_ptr<PlainConnection>` alive.

### Backend Destruction Order

`PlainConnection` destructor calls `close()` which calls `loop->remove_fd(fd)` which calls `backend_->remove_fd(fd)` and `clear_read_handler(fd)`. This is safe as long as the EventLoop outlives the connections — which it does (EventLoop is owned by ActorSystem, connections are managed by ConnectionPool which is stopped before EventLoop).

---

## Testing Considerations

1. **Unit test: `test_reactor_read_handler`** — Register a pipe fd with a read handler, write data to the other end, verify the handler is called via `wait()`.

2. **Unit test: `test_plain_connection_read`** — Create a socket pair, create a PlainConnection on one end, write a framed message on the other, verify `frame_handler_` is called.

3. **Integration test** — Existing `test_tcp_transport_comprehensive` should pass (or be updated) for plain text transport with actual data flow.

4. **Regression** — Existing TLS tests must still pass (TLS path is unchanged).

---

## Dependencies

- No new external dependencies
- No new files — all changes are in existing source files
- Proactor path deferred to follow-up (not needed for current use cases)

---

## Open Questions

1. **Buffer size**: 64KB stack buffer per read — reasonable for actor messages. Should it be configurable?
2. **Proactor completion routing**: Proactor completions carry an `ActorId` for routing. Should `PlainConnection` be assigned an internal ActorId, or should a different routing mechanism be used?
3. **readv vs read**: Currently using `::read()`. `::readv()` would allow scatter-gather into the read_buffer_ directly. Not needed for correctness, but could avoid a copy.
