# Zero-Copy Read Path Implementation Plan

**Date:** 2026-04-28
**Owner:** SKG7ON
**Status:** Planned
**Based on:** `docs/superpowers/specs/2026-04-28-zero-copy-read-path-span.md`

---

## Phase 0: Type Changes (Foundation)

### Task 0.1: `read_callback` → `void(int fd)` in async_io_fwd.hpp
**File:** `include/hpactor/net/async_io_fwd.hpp:34`
- [ ] `using read_callback = std::function<void(int fd)>;`
- [ ] Remove `const bytes&` parameter; callback does the read.

### Task 0.2: `frame_handler` → span in transport.hpp
**File:** `include/hpactor/net/transport.hpp:60`
- [ ] `using frame_handler = std::function<void(std::span<const uint8_t>)>;`

---

## Phase 1: service_read_handler — Minimal Dispatch

### Task 1.1: EpollBackend service_read_handler
**File:** `src/net/reactor/epoll_backend.cpp`
- [ ] Remove stack buffer, read loop, `bytes accumulated`.
- [ ] Just: look up callback, invoke `cb(fd)`.

### Task 1.2: KqueueBackend service_read_handler
**File:** `src/net/reactor/kqueue_backend.cpp`
- [ ] Same as EpollBackend.

---

## Phase 2: PlainConnection — Direct Read + Span Extraction

### Task 2.1: Declare `on_fd_readable` in header
**File:** `include/hpactor/net/plain_connection.hpp`
- [ ] Rename `handle_read(const bytes&)` → `on_fd_readable(int fd)`

### Task 2.2: Implement on_fd_readable
**File:** `src/net/plain_connection.cpp`
- [ ] Read loop: `::read(fd, read_buffer_.data() + off, ...)` — direct into buffer.
- [ ] Frame extraction: offset tracking with `std::span<const uint8_t>` views.
- [ ] Pass span to `frame_handler_`.

### Task 2.3: Update create_server / create_client lambdas
**File:** `src/net/plain_connection.cpp`
- [ ] Lambda: `[weak_conn](int fd) { if (auto self = weak_conn.lock()) self->on_fd_readable(fd); }`

---

## Phase 3: ConnectionPool and WireFrame

### Task 3.1: ConnectionPool::on_frame_received → span
**File:** `include/hpactor/net/connection_pool.hpp`, `src/net/connection_pool.cpp`
- [ ] Signature: `void on_frame_received(std::span<const uint8_t> frame_data);`
- [ ] Call `WireFrame::decode(span)`.

### Task 3.2: WireFrame::decode(span) overload
**File:** `include/hpactor/net/frame.hpp`, `src/net/frame.cpp`
- [ ] Add `static WireFrame decode(std::span<const uint8_t> data);`
- [ ] Parse header fields from span. Payload is a subspan.

### Task 3.3: Update ConnectionPool frame lambdas
**File:** `src/net/connection_pool.cpp`
- [ ] `set_frame_handler` lambda: `[this](std::span<const uint8_t> data) { on_frame_received(data); }`

---

## Phase 4: Downstream Consumers

### Task 4.1: TcpTransport handler lambdas → span
**File:** `src/net/tcp_transport.cpp`
- [ ] `set_frame_handler` lambdas accept `std::span<const uint8_t>`.

### Task 4.2: ActorSystem::deliver_remote — ownership boundary copy
**File:** `src/actor/actor_system.cpp`
- [ ] `TypedMessage` construction from span payload — this is Copy 2.

### Task 4.3: Test files
**Files:** Tests that create frame_handler or read_callback lambdas.
- [ ] Update lambda signatures.

---

## Phase 5: Build, Test, Verify

### Task 5.1: Build
**Command:** `cmake -S . -B build -GNinja && ninja -C build`
**Expected:** Clean build.

### Task 5.2: Run tests
**Command:** `ctest --output-on-failure`
**Expected:** 61/61 passing.

---

## Dependency Graph

```
Phase 0 (Types) ──┬── Phase 1 (service_read_handler, both backends)
                   ├── Phase 2 (PlainConnection)
                   ├── Phase 3 (ConnectionPool + WireFrame)
                   └── Phase 4 (Downstream: tcp_transport, actor_system, tests)
                                      │
                                      └── Phase 5 (Build, test, verify)
```

Phase 0 first. Phases 1-4 independent after Phase 0.

---

## File Summary

| File | Phase | Scope |
|------|-------|-------|
| `include/hpactor/net/async_io_fwd.hpp` | 0.1 | `read_callback` → `void(int fd)` |
| `include/hpactor/net/transport.hpp` | 0.2 | `frame_handler` → span |
| `src/net/reactor/epoll_backend.cpp` | 1.1 | `service_read_handler` → `cb(fd)` only |
| `src/net/reactor/kqueue_backend.cpp` | 1.2 | `service_read_handler` → `cb(fd)` only |
| `include/hpactor/net/plain_connection.hpp` | 2.1 | `handle_read` → `on_fd_readable(int fd)` |
| `src/net/plain_connection.cpp` | 2.2, 2.3 | Direct read into buffer, span extraction, lambdas |
| `include/hpactor/net/connection_pool.hpp` | 3.1 | `on_frame_received` → span |
| `src/net/connection_pool.cpp` | 3.1, 3.3 | Span decode, lambda updates |
| `include/hpactor/net/frame.hpp` | 3.2 | `WireFrame::decode(span)` |
| `src/net/frame.cpp` | 3.2 | Span decode implementation |
| `src/net/tcp_transport.cpp` | 4.1 | Handler lambdas → span |
| `src/actor/actor_system.cpp` | 4.2 | `deliver_remote` ownership boundary copy |
| Test files | 4.3 | Signature updates |
