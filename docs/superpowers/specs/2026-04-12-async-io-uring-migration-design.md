# Async I/O Backend Migration: io_uring + libdispatch

## Status

Design approved 2026-04-12. Pending implementation.

## 1. Motivation

The current `src/net/` networking stack uses kqueue (macOS) / epoll (Linux) for I/O event notification via `EventLoop`. However:

- `TlsConnection::send_raw()` (tls_connection.cpp:442) is **synchronous** `::send()` — not async at all
- `on_fd_readable()` / `on_fd_writable()` are **stubs**
- `RegistrarServer` and `RegistrarClient` run **blocking threads with `select()`**, completely outside the EventLoop
- The reactive epoll/kqueue model requires a syscall per operation submitted

The goal is to replace the reactive (epoll/kqueue) model with **proactive io_uring** on Linux, providing true async I/O with submission-queue (SQ) batching, and to provide a similarly async experience on macOS via libdispatch.

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                     TcpTransport                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Connection  │  │  Connection  │  │  ...more     │  │
│  │   Pool A     │  │   Pool B     │  │   pools      │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│                          │                              │
│  ┌──────────────────────────────────────────────────┐  │
│  │              AsyncIoBackend (abstract)             │  │
│  └──────────────────────────────────────────────────┘  │
│                    /          \                        │
│           ┌──────────────┐  ┌──────────────┐           │
│           │  IoUring     │  │  Gcdispatch  │           │
│           │  (Linux)     │  │  (macOS)     │           │
│           └──────────────┘  └──────────────┘           │
└─────────────────────────────────────────────────────────┘
```

- `AsyncIoBackend` is the platform-agnostic abstract interface
- `IoUringBackend` on Linux implements full io_uring (proactive SQ/CQ model)
- `GcdBackend` on macOS uses libdispatch async socket operations (reactive model — inherent Apple limitation)
- Both implement the same `AsyncIoBackend` interface; upper layers (TlsConnection, ConnectionPool, Registrar) are unchanged

## 3. AsyncIoBackend Interface

### Opaque Timer Handle
Timer handles are backend-internal and comparison-equivalent only within the same `AsyncIoBackend` instance. Callers store handles as `uint64_t` (as returned) and pass them back to `cancel_timer()` on the same backend instance.

### Interface

```cpp
class AsyncIoBackend {
public:
    virtual ~AsyncIoBackend() = default;

    // --- Lifecycle ---
    virtual bool start() = 0;
    virtual void stop() = 0;

    // --- File descriptor registration ---
    virtual bool add_fd(int fd, IoEvent events) = 0;
    virtual bool update_fd(int fd, IoEvent events) = 0;
    virtual bool remove_fd(int fd) = 0;

    // --- Fixed buffer registration ---
    // Registers a memory region for use with async_send_fixed/async_recv_fixed.
    // Returns a buffer_id (index into backend's registered buffer array) or -1 on error.
    // The buffer must remain valid until unregistered.
    virtual int register_buffer(const void* addr, size_t len) = 0;
    virtual bool unregister_buffer(int buffer_id) = 0;

    // --- Async I/O: vectored (uses WRITEV/READV, no fixed buffer) ---
    // user_data carries ActorId + op type for completion routing
    virtual void async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;

    // --- Async I/O: fixed buffer (uses WRITE_FIXED/READ_FIXED) ---
    // buffer_id is index into pre-registered buffer array
    virtual void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                   ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                   ActorId actor, uint32_t op_type) = 0;

    // --- Connection operations ---
    virtual void async_accept(int fd, ActorId actor) = 0;
    virtual void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                               ActorId actor) = 0;

    // --- UDP operations (for RegistrarClient node resolution) ---
    // Note: on macOS, libdispatch dispatch_read on UDP does not expose source
    // address; callers needing peer address must use getpeername() after receive.
    virtual void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                 ActorId actor, uint32_t op_type) = 0;
    virtual void async_sendto(int fd, const iovec* bufs, int buf_count,
                               const sockaddr* addr, socklen_t addrlen,
                               ActorId actor, uint32_t op_type) = 0;

    // --- Timer operations (via io_uring timeouts on Linux) ---
    // TimerHandle is backend-internal; uint64_t is comparison-equivalent within
    // a single backend instance only.
    virtual uint64_t run_after(ActorId actor, int delay_ms) = 0;
    virtual uint64_t run_every(ActorId actor, int interval_ms) = 0;
    virtual void cancel_timer(uint64_t handle) = 0;

    // --- Event loop pump ---
    virtual int wait(int timeout_ms) = 0;   // returns triggered event count
    virtual void process_completions() = 0; // drain CQ into actor messages
};
```

- All async operations are **non-blocking**; results come asynchronously via actor message delivery
- `user_data` encodes `ActorId` and `op_type` so the completion handler routes the result to the correct actor
- Two tiers of async I/O: **vectored** (`async_send`/`async_recv` via `WRITEV`/`READV`) and **fixed-buffer** (`async_send_fixed`/`async_recv_fixed` via `WRITE_FIXED`/`READ_FIXED`). Fixed buffer operations require buffers to be pre-registered with the backend.
- Linked SQEs are used for compound operations (e.g., send + recv as one atomic chain)

## 4. IoUringBackend — Linux Implementation

### Proactive Model
Uses `io_uring_setup()` with `IORING_SETUP_SQPOLL` for proactive (kernel-side busy-polling) mode. When the application is idle, the kernel polls the SQ head — no userspace polling needed, no syscalls until work arrives.

### Fixed Buffers
Fixed buffers are registered via `register_buffer()` which calls `io_uring_register()` with `IORING_REGISTER_BUFFERS`. The returned `buffer_id` is the index into the registered buffer array, used by `async_send_fixed()` / `async_recv_fixed()` with `IORING_OP_WRITE_FIXED` / `IORING_OP_READ_FIXED`. Eliminates per-operation `malloc`/`free` in the fast path.

### File Descriptor Registration
Socket file descriptors pre-registered via `io_uring_register()` with `IORING_REGISTER_FILES`. Avoids `O_NONBLOCK` setup per socket.

### Async Operations

| Operation | io_uring Opcode | Notes |
|-----------|-----------------|-------|
| Send (vectored) | `IORING_OP_WRITEV` | Uses iovec, no fixed buffer |
| Send (fixed) | `IORING_OP_WRITE_FIXED` | Uses pre-registered buffer ID |
| Recv (vectored) | `IORING_OP_READV` | Uses iovec, no fixed buffer |
| Recv (fixed) | `IORING_OP_READ_FIXED` | Uses pre-registered buffer ID |
| Accept | `IORING_OP_ACCEPT` | Direct async accept |
| Connect | `IORING_OP_CONNECT` | Direct async connect |
| RecvFrom | `IORING_OP_RECVMSG` | UDP receive with address |
| SendTo | `IORING_OP_SENDMSG` | UDP send with address |
| Timeout | `IORING_OP_TIMEOUT` | Monotonic clock, `clockid=CLOCK_MONOTONIC` |

### Linked SQEs
Used during handshake and compound operations:
- Client: send `ClientHello` (chained with) recv `ServerHello` — one `io_uring_enter()` submit
- Server: accept connection (chained with) recv `ClientHello`

### Error Handling
- **Submission failures** (can't queue SQE) are **fatal bugs** — abort program
- **Completion failures** (CQE with `res < 0`) are delivered via actor message with error code

## 5. GcdBackend — macOS Implementation

### libdispatch Async Operations
Uses Apple's **Grand Central Dispatch** (libdispatch) async socket APIs:

| Operation | libdispatch API |
|-----------|----------------|
| Send | `dispatch_write()` |
| Recv | `dispatch_read()` |
| Accept | `dispatch_source` with `DISPATCH_SOURCE_TYPE_READ` on listen fd |
| Connect | `dispatch_connect()` |
| RecvFrom | `dispatch_source` with `DISPATCH_SOURCE_TYPE_READ` on UDP socket |
| SendTo | `dispatch_write()` to UDP socket with address |
| Timer | `dispatch_after()` / `dispatch_source` with `DISPATCH_SOURCE_TYPE_TIMER` |

### Fixed Buffers
Via `dispatch_data_make()` with `DISPATCH_DATA_DESTRUCTOR_DEFAULT`.

### Key Limitation
Unlike io_uring's proactive submission-queue model, libdispatch on macOS is fundamentally **completion-based/reactive** — the kernel notifies when an operation is ready, and libdispatch invokes a callback. There is no equivalent to io_uring's submission-side batching or SQPOLL on Apple platforms. This is an inherent architectural difference, not a code limitation.

### Error Handling
Same as IoUringBackend: submission errors are fatal; completion errors are delivered as messages to the actor.

## 6. TlsConnection Integration

### Before (Synchronous)
```cpp
void TlsConnection::send_raw(const bytes& data) {
    if (fd_ < 0) return;
    ::send(fd_, data.data(), data.size(), 0);  // blocks, no yield
}
```

### After (Async)
```cpp
void TlsConnection::send_raw(const bytes& data) {
    if (fd_ < 0) return;
    // Use fixed-buffer path for performance; buffer_id pre-registered
    loop_->async_send_fixed(fd_, buffer_id_, 0, data.size(),
                            actor_id_, OpType::Send);
    // Actor yields — Behavior suspends
    // Later: CQE fires → message delivered to actor
    // Actor's current Behavior resumes, handles send completion
}
```

### Flow
1. `TlsConnection` calls `loop_->async_send()` with its actor ID and `OpType::Send`
2. Actor's current `Behavior` continues processing other messages or yields
3. `EventLoop::process_completions()` drains CQEs after each `wait()` cycle
4. For each CQE, `user_data` is decoded to `(ActorId, OpType)` and a completion message is posted to that actor's mailbox
5. The actor's `Behavior` handles the I/O completion as a normal message

### Linked SQEs During Handshake
- Client: submits `WRITE_FIXED(ClientHello)` linked with `READ_FIXED(ServerHello buffer)` via `IOSQE_IO_LINK` flag in one `io_uring_enter()` call
- Server: submits `ACCEPT` linked with `READ_FIXED(ClientHello buffer)` in one call

## 7. Registrar Migration

### RegistrarServer (was: `select()` in dedicated thread)

**Before:**
```cpp
// Dedicated thread
while (running) {
    fd_set readfds;
    // select() on server fd and all client fds
    select(max_fd + 1, &readfds, nullptr, nullptr, nullptr);
    // handle accepts and reads
}
```

**After:**
- Listen fd registered with EventLoop via `async_accept()`
- Each newly accepted connection registers its fd with EventLoop
- All read/write operations go through `async_recv()` / `async_send()`
- `user_data` routes completions to `RegistrarActor` by actor ID
- `RegistrarActor` handles `NodeJoin`, `NodeLeave`, `Heartbeat`, `Register` as `Behavior` message handlers

### RegistrarClient (was: blocking threads with `select()`)

**Before:**
```cpp
// Thread 1: blocking select() on server TCP connection
// Thread 2: blocking select() on UDP probe socket
// Thread 3: timer-based heartbeat
```

**After:**
- `async_connect()` for server connection — completion via actor message
- `async_recv()` on server fd — messages delivered to `RegistrarClientActor`
- `async_send()` for outgoing messages
- `run_every()` via EventLoop (timer) — heartbeat delivered as actor message
- `resolve_node()` UDP query (if needed) via `async_recvfrom()` / `async_sendto()`
- Dedicated connection/heartbeat threads eliminated

## 8. Build / Dependency Changes

### CMake

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(liburing REQUIRED)
    target_link_libraries(hpactor_lib PUBLIC liburing::uring)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_library(dispatch dispatch)
    if(NOT dispatch)
        message(FATAL_ERROR "libdispatch not found on macOS")
    endif()
    target_link_libraries(hpactor_lib PUBLIC ${dispatch})
endif()
```

### New Files
| File | Purpose |
|------|---------|
| `include/hpactor/net/async_io_backend.hpp` | Abstract interface |
| `include/hpactor/net/iouring_backend.hpp` | Linux io_uring backend |
| `include/hpactor/net/gcd_backend.hpp` | macOS libdispatch backend |
| `src/net/iouring_backend.cpp` | io_uring implementation |
| `src/net/gcd_backend.cpp` | libdispatch implementation |

### Modified Files
| File | Change |
|------|--------|
| `src/net/event_loop.cpp` | Delegates to platform-specific `AsyncIoBackend` |
| `src/net/tcp_transport.cpp` | Creates platform-specific backend |
| `src/net/tls_connection.cpp` | `send_raw()` async, `on_fd_readable/writable` wired |
| `src/net/registrar_server.cpp` | Migrate off blocking thread/select |
| `src/net/registrar_client.cpp` | Migrate off blocking threads/select |
| `CMakeLists.txt` | Added liburing/libdispatch dependencies |

## 9. Design Decisions Summary

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Backend abstraction | `AsyncIoBackend` interface | Platform-agnostic upper layers |
| Linux backend | io_uring (liburing) | Proactive SQ/CQ model, zero-copy via fixed buffers |
| macOS backend | libdispatch | Native Apple async I/O framework |
| Linux mode | `IORING_SETUP_SQPOLL` | Proactive polling, no userspace busy-wait |
| Buffer API | Two-tier: vectored (`WRITEV`/`READV`) + fixed (`WRITE_FIXED`/`READ_FIXED`) | Simple vectored path for flexibility; fixed path for performance |
| Linked SQEs | Yes | Compound ops (send+recv, accept+recv) atomic |
| Timer mechanism | io_uring timeout (Linux), dispatch_source (macOS) | Unified timer interface |
| Timer handle | `uint64_t`, backend-internal encoding | Callers treat as opaque; comparison only within same backend |
| Actor integration | `user_data` → actor ID → actor mailbox | Fits existing Behavior/message model |
| Completion model | Actor messages (no direct callbacks) | Consistent with HPActor actor model |
| Error handling | Submission failure = fatal; completion error = message | Distinguishes programmer error from runtime |
| RegistrarServer | Event-driven acceptor (no dedicated thread) | Pure async, kernel wakes EventLoop |
| RegistrarClient | All I/O via EventLoop, heartbeat via `run_every()` | Blocking threads eliminated |
