# Unified Read Abstraction Implementation Plan

**Date:** 2026-04-28
**Owner:** SKG7ON
**Status:** Planned
**Based on:** `docs/superpowers/specs/2026-04-28-unified-read-abstraction-reactor-proactor.md`

---

## Phase 0: Interface — Add `supports_read_handler()`

### Task 0.1: Add `supports_read_handler()` to IReactorBackend
**File:** `include/hpactor/net/reactor_backend.hpp`
**Dependencies:** None
- [ ] Add `virtual bool supports_read_handler() const = 0;` to `IReactorBackend`
- Pure virtual — all backends must implement

### Task 0.2: Verify EventLoop delegates `set_read_handler` correctly
**File:** `src/net/event_loop.cpp`, `include/hpactor/net/event_loop.hpp`
**Dependencies:** Task 0.1
- [ ] Un-stub `EventLoop::set_read_handler`: delegate to `backend_->set_read_handler(fd, handler)`
- [ ] Un-stub `EventLoop::clear_read_handler`: delegate to `backend_->clear_read_handler(fd)`
- [ ] Add `bool EventLoop::supports_read_handler() const` delegating to backend

---

## Phase 1: EpollBackend — Implement Read Handler Support

### Task 1.1: Add data members and override declarations
**File:** `include/hpactor/net/reactor/epoll_backend.hpp`
**Dependencies:** Task 0.1
- [ ] Add `std::unordered_map<int, read_callback> read_handlers_;`
- [ ] Override `bool supports_read_handler() const override { return true; }`
- [ ] Override `void set_read_handler(int fd, read_callback handler) override;`
- [ ] Override `void clear_read_handler(int fd) override;`
- [ ] Declare private helper `void service_read_handler(int fd);`

### Task 1.2: Implement set_read_handler / clear_read_handler
**File:** `src/net/reactor/epoll_backend.cpp`
**Dependencies:** Task 1.1
- [ ] `set_read_handler`: store callback in `read_handlers_[fd]`; if handler is null, erase
- [ ] `clear_read_handler`: erase from `read_handlers_`

### Task 1.3: Implement service_read_handler
**File:** `src/net/reactor/epoll_backend.cpp`
**Dependencies:** Task 1.2
- [ ] Allocate 64KB stack buffer
- [ ] `::read(fd, buf, sizeof(buf))` in loop until EAGAIN/EWOULDBLOCK
- [ ] On data: construct `bytes` and call stored callback
- [ ] On EOF (return 0) or error: break out of loop

### Task 1.4: Wire service_read_handler into wait()
**File:** `src/net/reactor/epoll_backend.cpp` — `wait()` method
**Dependencies:** Task 1.3
- [ ] In the `wait()` event loop, after `process_pending_op(fd, events)` returns without finding a pending op:
  - Check if fd is in `read_handlers_`
  - If yes, call `service_read_handler(fd)`
- [ ] Ensure `remove_fd` also clears any read handler for that fd

---

## Phase 2: KqueueBackend — Implement Read Handler Support

### Task 2.1: Add data members and override declarations
**File:** `include/hpactor/net/reactor/kqueue_backend.hpp`
**Dependencies:** Task 0.1
- [ ] Add `std::unordered_map<int, read_callback> read_handlers_;`
- [ ] Override `bool supports_read_handler() const override { return true; }`
- [ ] Override `void set_read_handler(int fd, read_callback handler) override;`
- [ ] Override `void clear_read_handler(int fd) override;`
- [ ] Declare private helper `void service_read_handler(int fd);`

### Task 2.2: Implement set_read_handler / clear_read_handler
**File:** `src/net/reactor/kqueue_backend.cpp`
**Dependencies:** Task 2.1
- [ ] Same pattern as EpollBackend

### Task 2.3: Implement service_read_handler
**File:** `src/net/reactor/kqueue_backend.cpp`
**Dependencies:** Task 2.2
- [ ] Same pattern as EpollBackend: stack buffer, `::read()` loop, call callback

### Task 2.4: Wire service_read_handler into wait()
**File:** `src/net/reactor/kqueue_backend.cpp` — `wait()` method
**Dependencies:** Task 2.3
- [ ] In the `wait()` event processing loop (iterating `events_[0..last_num_events_]`):
  - After handling accept actors and pending recv ops, check `read_handlers_` for remaining EVFILT_READ events
  - Call `service_read_handler(fd)` for matching fds
- [ ] Ensure `remove_fd` clears any read handler for that fd

---

## Phase 3: Proactor Backends — Return false

### Task 3.1: IoUringBackend
**File:** `include/hpactor/net/proactor/iouring_backend.hpp`
**Dependencies:** Task 0.1
- [ ] `bool supports_read_handler() const override { return false; }`
- [ ] Keep existing `set_read_handler`/`clear_read_handler` as no-ops (correct for proactor)

### Task 3.2: GcdBackend
**File:** `include/hpactor/net/proactor/gcd_backend.hpp`
**Dependencies:** Task 0.1
- [ ] `bool supports_read_handler() const override { return false; }`
- [ ] Keep existing `set_read_handler`/`clear_read_handler` as no-ops (correct for proactor)

---

## Phase 4: PlainConnection — Wire Read Handler

### Task 4.1: Wire set_read_handler in create_server
**File:** `src/net/plain_connection.cpp` — `create_server()`
**Dependencies:** Phase 1, Phase 2
- [ ] After `loop->add_fd(fd, Read)`:
  - Check `loop->supports_read_handler()`
  - If true: capture `weak_ptr<PlainConnection>` and set read handler calling `handle_read(data)`
  - If false: leave a TODO comment for proactor async_recv path
- [ ] Use `weak_ptr` to avoid circular reference (EventLoop → lambda → shared_ptr → EventLoop)

### Task 4.2: Wire set_read_handler in create_client
**File:** `src/net/plain_connection.cpp` — `create_client()`
**Dependencies:** Task 4.1
- [ ] Same pattern as `create_server`: register fd + set read handler
- [ ] Client fds are already non-blocking and connected

### Task 4.3: Clean up read handler on close
**File:** `src/net/plain_connection.cpp` — `close()`
**Dependencies:** Task 4.1, 4.2
- [ ] Before `::close(fd)`, call `loop->clear_read_handler(fd)` (or rely on `remove_fd` cleanup)
- [ ] Verify `loop->remove_fd(fd)` also clears the read handler in the backend

---

## Phase 5: TcpTransport — Wire Server-Side Handlers

### Task 5.1: Wire frame_handler and error_handler in handle_accept
**File:** `src/net/tcp_transport.cpp` — `handle_accept()`
**Dependencies:** Phase 4
- [ ] After creating server-side `PlainConnection`:
  - Get or create pool for `remote_endpoint`
  - Set `frame_handler` → `pool->on_frame_received(data)`
  - Set `error_handler` → `pool->on_connection_error(conn, err)`
  - Call `pool->add_connection(conn)`
- [ ] Same wiring already exists for client-side PlainConnection in `connect()` — mirror it

---

## Phase 6: Build, Test, Verify

### Task 6.1: Build
**Command:** `cmake -S . -B build -GNinja && ninja -C build`
**Dependencies:** Phase 0–5
**Expected:** Clean build, no regressions

### Task 6.2: Run existing tests
**Command:** `ctest --output-on-failure`
**Dependencies:** Task 6.1
**Expected:** All 61 tests pass, no regressions

### Task 6.3: Manual integration smoke test
**Dependencies:** Task 6.2
- [ ] Verify plain text connection can send and receive a framed message
- [ ] Verify TLS connection path is unchanged

---

## Dependency Graph

```
Phase 0 (Interface)
  │
  ├── Phase 1 (EpollBackend)
  │     Task 1.1 → 1.2 → 1.3 → 1.4
  │
  ├── Phase 2 (KqueueBackend)  [parallel with Phase 1]
  │     Task 2.1 → 2.2 → 2.3 → 2.4
  │
  ├── Phase 3 (Proactor stubs)  [parallel with Phase 1, 2]
  │     Task 3.1, 3.2 (independent, one-line each)
  │
  └── Phase 4 (PlainConnection)
        Task 4.1 → 4.2 → 4.3
              │
              └── Phase 5 (TcpTransport)
                    Task 5.1
                          │
                          └── Phase 6 (Verify)
                                Task 6.1 → 6.2 → 6.3
```

Phases 1, 2, and 3 are independent and can be done in parallel. Phase 4 depends on 1+2 being complete. Phase 5 depends on 4.

---

## File Summary

| File | Phase | Scope |
|------|-------|-------|
| `include/hpactor/net/reactor_backend.hpp` | 0 | Add 1 pure virtual method |
| `include/hpactor/net/event_loop.hpp` | 0 | Un-stub 2 method declarations, add 1 |
| `src/net/event_loop.cpp` | 0 | Implement 3 methods (delegate to backend) |
| `include/hpactor/net/reactor/epoll_backend.hpp` | 1 | Add 4 declarations, 1 data member |
| `src/net/reactor/epoll_backend.cpp` | 1 | Implement ~50 lines (set/clear/service, wait wiring) |
| `include/hpactor/net/reactor/kqueue_backend.hpp` | 2 | Add 4 declarations, 1 data member |
| `src/net/reactor/kqueue_backend.cpp` | 2 | Implement ~50 lines (set/clear/service, wait wiring) |
| `include/hpactor/net/proactor/iouring_backend.hpp` | 3 | 1 line: `return false` |
| `include/hpactor/net/proactor/gcd_backend.hpp` | 3 | 1 line: `return false` |
| `src/net/plain_connection.cpp` | 4 | Wire read handler in create_server + create_client |
| `src/net/tcp_transport.cpp` | 5 | Wire frame/error handlers in handle_accept |
| No new files | — | All changes in existing sources |

---

## Detailed Work Tasks

### Phase 0: Interface

| # | Task | File | Action |
|---|------|------|--------|
| 0.1 | Add `supports_read_handler()` pure virtual | `reactor_backend.hpp:53` | Insert `virtual bool supports_read_handler() const = 0;` after `clear_read_handler` |
| 0.2 | Un-stub `EventLoop::set_read_handler` | `event_loop.cpp:130-132` | Replace no-op body with `if (backend_) backend_->set_read_handler(fd, std::move(handler));` |
| 0.3 | Un-stub `EventLoop::clear_read_handler` | `event_loop.cpp:134-136` | Replace no-op body with `if (backend_) backend_->clear_read_handler(fd);` |
| 0.4 | Add `EventLoop::supports_read_handler()` | `event_loop.hpp` + `event_loop.cpp` | Declaration + `return backend_ && backend_->supports_read_handler();` |

### Phase 1: EpollBackend

| # | Task | File | Action |
|---|------|------|--------|
| 1.1 | Add `read_handlers_` map + overrides | `epoll_backend.hpp` | Add `std::unordered_map<int, read_callback> read_handlers_;`, override declarations for `supports_read_handler`, `set_read_handler`, `clear_read_handler`, private `service_read_handler` |
| 1.2 | Implement `set_read_handler` | `epoll_backend.cpp` | Store/erase callback in `read_handlers_` map |
| 1.3 | Implement `clear_read_handler` | `epoll_backend.cpp` | Erase from `read_handlers_` |
| 1.4 | Implement `service_read_handler` | `epoll_backend.cpp` | 64KB stack buffer, `::read()` loop until EAGAIN, call callback with `bytes` |
| 1.5 | Wire into `wait()` | `epoll_backend.cpp:wait()` | After `process_pending_op` returns without finding op: check `read_handlers_`, call `service_read_handler` |
| 1.6 | Clean up on `remove_fd` | `epoll_backend.cpp:remove_fd()` | Call `clear_read_handler(fd)` |

### Phase 2: KqueueBackend

| # | Task | File | Action |
|---|------|------|--------|
| 2.1 | Add `read_handlers_` map + overrides | `kqueue_backend.hpp` | Same as EpollBackend 1.1 |
| 2.2 | Implement `set_read_handler` | `kqueue_backend.cpp` | Store/erase callback in `read_handlers_` map |
| 2.3 | Implement `clear_read_handler` | `kqueue_backend.cpp` | Erase from `read_handlers_` |
| 2.4 | Implement `service_read_handler` | `kqueue_backend.cpp` | Same `::read()` loop pattern as EpollBackend |
| 2.5 | Wire into `wait()` event loop | `kqueue_backend.cpp:wait()` | In the `events_[0..last_num_events_]` loop, after accept + pending op handling, check `read_handlers_` for EVFILT_READ events, call `service_read_handler` |
| 2.6 | Clean up on `remove_fd` | `kqueue_backend.cpp:remove_fd()` | Call `clear_read_handler(fd)` |

### Phase 3: Proactor Stubs

| # | Task | File | Action |
|---|------|------|--------|
| 3.1 | `IoUringBackend::supports_read_handler` | `iouring_backend.hpp` | `return false;` |
| 3.2 | `GcdBackend::supports_read_handler` | `gcd_backend.hpp` | `return false;` |

### Phase 4: PlainConnection

| # | Task | File | Action |
|---|------|------|--------|
| 4.1 | Wire read handler in `create_server` | `plain_connection.cpp:46-60` | After `add_fd`: get `weak_ptr`, call `loop->set_read_handler(fd, lambda)` calling `handle_read` |
| 4.2 | Wire read handler in `create_client` | `plain_connection.cpp:36-44` | Same pattern: register fd + set read handler for reactor path |
| 4.3 | Ensure `close()` cleans up | `plain_connection.cpp:86-95` | Call `loop->clear_read_handler(fd)` before `remove_fd` (safety) |

### Phase 5: TcpTransport

| # | Task | File | Action |
|---|------|------|--------|
| 5.1 | Wire handlers in `handle_accept` | `tcp_transport.cpp:267-277` | After creating server-side PlainConnection: set frame_handler → pool->on_frame_received, set error_handler → pool->on_connection_error, add to pool |

### Phase 6: Verify

| # | Task | Command | Expected |
|---|------|---------|----------|
| 6.1 | Build | `cmake -S . -B build -GNinja && ninja -C build` | Clean, no errors |
| 6.2 | Run tests | `ctest --output-on-failure` | 61/61 passing |
| 6.3 | Sanitizer build | `cmake -DENABLE_TSAN=ON .. && ninja && ctest` | Clean, no races |

---

## Verification Checklist

After implementation, verify:
- [ ] `EpollBackend::supports_read_handler()` returns true
- [ ] `KqueueBackend::supports_read_handler()` returns true
- [ ] `IoUringBackend::supports_read_handler()` returns false
- [ ] `GcdBackend::supports_read_handler()` returns false
- [ ] `EventLoop::set_read_handler` delegates to backend (no longer no-op)
- [ ] `PlainConnection::create_server` registers read handler on reactor backends
- [ ] `PlainConnection::create_client` registers read handler on reactor backends
- [ ] Data arriving on a PlainConnection socket reaches `handle_read`
- [ ] `handle_read` framing works — complete frames dispatched to `frame_handler_`
- [ ] Server-accepted PlainConnections route data through ConnectionPool
- [ ] TLS path is unchanged and still passes all TLS tests
- [ ] All 61 existing tests still pass
- [ ] ThreadSanitizer build is clean
