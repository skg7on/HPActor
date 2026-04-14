# EventLoop Backend Fallback Implementation Plan

**Date:** 2026-04-14
**Owner:** SKG7ON
**Status:** Planned
**Based on:** `docs/superpowers/specs/2026-04-14-event-loop-backend-fallback-design.md`

---

## Phase 0: Research & Analysis

### Task 0.1: Study Existing Backend Implementations
**Files:** `src/net/iouring_backend.cpp`, `src/net/gcd_backend.cpp`
**Dependencies:** None
**Tasks:**
- [ ] Understand how GcdBackend implements all AsyncIoBackend methods
- [ ] Understand how IoUringBackend implements all AsyncIoBackend methods
- [ ] Identify common patterns that can be shared/abstracted
- [ ] Note any platform-specific assumptions

### Task 0.2: Identify EventLoop Usage Sites
**Command:** `grep -r "EventLoop" --include="*.cpp" --include="*.hpp" | grep -v test`
**Dependencies:** Task 0.1
**Tasks:**
- [ ] List all files that construct EventLoop
- [ ] List all files that call EventLoop methods (add_fd, wait, etc.)
- [ ] Identify any immediate use after construction (will need `run()` call added)

---

## Phase 1: Create EpollBackend

### Task 1.1: Create EpollBackend Header
**File:** `include/hpactor/net/epoll_backend.hpp`
**Dependencies:** Task 0.1
**Tasks:**
- [ ] Define `EpollBackend` class inheriting from `AsyncIoBackend`
- [ ] Declare all required virtual methods (start, stop, add_fd, update_fd, remove_fd, register_buffer, unregister_buffer, async_send, async_recv, async_send_fixed, async_recv_fixed, async_accept, async_connect, async_recvfrom, async_sendto, run_after, run_every, cancel_timer, wait, process_completions, deliver_completion)
- [ ] Declare private members: epoll_fd_, timerfd_, timers_, next_timer_handle_, fd_events_

### Task 1.2: Create EpollBackend Implementation Skeleton
**File:** `src/net/epoll_backend.cpp`
**Dependencies:** Task 1.1
**Tasks:**
- [ ] Include headers: `<sys/epoll.h>`, `<sys/timerfd.h>`, `<unistd.h>`, `<fcntl.h>`
- [ ] Implement constructor: create epoll_fd_ with `epoll_create1(EPOLL_CLOEXEC)`, create timerfd_
- [ ] Implement destructor: close epoll_fd_, timerfd_
- [ ] Implement `start()`: return true if epoll_fd_ valid
- [ ] Implement `stop()`: clean up timers, close fds

### Task 1.3: Implement EpollBackend FD Operations
**File:** `src/net/epoll_backend.cpp`
**Dependencies:** Task 1.2
**Tasks:**
- [ ] Implement `add_fd()`: use `epoll_ctl(ADD)` with EPOLLIN/EPOLLOUT
- [ ] Implement `update_fd()`: use `epoll_ctl(MOD)`
- [ ] Implement `remove_fd()`: use `epoll_ctl(DEL)`

### Task 1.4: Implement EpollBackend Timers
**File:** `src/net/epoll_backend.cpp`
**Dependencies:** Task 1.3
**Tasks:**
- [ ] Add timerfd_ to epoll interest list with EPOLLIN
- [ ] Implement `run_after()`: calculate expiry, add to timers map, return handle
- [ ] Implement `run_every()`: same but mark as repeating
- [ ] Implement `cancel_timer()`: remove from timers map
- [ ] In `wait()`: check timer expirations, fire timers, return triggered count

### Task 1.5: Implement EpollBackend Async Operations (Stub)
**File:** `src/net/epoll_backend.cpp`
**Dependencies:** Task 1.4
**Tasks:**
- [ ] For async_send/async_recv: use non-blocking posix send/recv with thread pool OR return error for now (not critical for basic functionality)
- [ ] async_accept: use non-blocking accept() + epoll monitoring
- [ ] async_connect: use non-blocking connect() + epoll monitoring
- [ ] async_sendto/async_recvfrom: similar pattern with UDP
- [ ] async_send_fixed/async_recv_fixed: return error (not supported with epoll)
- [ ] register_buffer/unregister_buffer: return -1 (not supported)

### Task 1.6: Implement EpollBackend process_completions
**File:** `src/net/epoll_backend.cpp`
**Dependencies:** Task 1.5
**Tasks:**
- [ ] Read timerfd_ to consume expired timer events
- [ ] Process any completed async operations
- [ ] Call `deliver_completion()` for each completed operation

---

## Phase 2: Create KqueueBackend

### Task 2.1: Create KqueueBackend Header
**File:** `include/hpactor/net/kqueue_backend.hpp`
**Dependencies:** Task 0.1
**Tasks:**
- [ ] Define `KqueueBackend` class inheriting from `AsyncIoBackend`
- [ ] Declare all required virtual methods (same as EpollBackend)
- [ ] Declare private members: kqueue_fd_, timers_, next_timer_handle_, fd_events_

### Task 2.2: Create KqueueBackend Implementation
**File:** `src/net/kqueue_backend.cpp`
**Dependencies:** Task 2.1
**Tasks:**
- [ ] Include headers: `<sys/event.h>`, `<sys/time.h>`, `<unistd.h>`
- [ ] Implement constructor: create kqueue_fd_ with `kqueue()`
- [ ] Implement destructor: close kqueue_fd_
- [ ] Implement `add_fd()`: use `kevent(ADD)` with EVFILT_READ/EVFILT_WRITE
- [ ] Implement `update_fd()`: use `kevent(ADD)` with updated flags
- [ ] Implement `remove_fd()`: use `kevent(DELETE)`
- [ ] Implement timers using EVFILT_TIMER
- [ ] Stub async operations (similar to EpollBackend)

---

## Phase 3: Update EventLoop

### Task 3.1: Update EventLoop Header
**File:** `include/hpactor/net/event_loop.hpp`
**Dependencies:** Phase 1, Phase 2
**Tasks:**
- [ ] Add `bool run()` method declaration
- [ ] Add `void stop()` method declaration
- [ ] Update comments to document explicit lifecycle

### Task 3.2: Create Backend Factory Helper
**File:** `src/net/event_loop.cpp`
**Dependencies:** Task 3.1
**Tasks:**
- [ ] Add anonymous namespace with `try_create<T>()` template function
- [ ] Function attempts to create backend, calls start(), returns nullptr on failure

### Task 3.3: Update EventLoop Constructor
**File:** `src/net/event_loop.cpp`
**Dependencies:** Task 3.2
**Tasks:**
- [ ] Remove auto-start from constructor
- [ ] Use backend factory with fallback chain
- [ ] On Linux: try IoUringBackend → EpollBackend
- [ ] On macOS: try GcdBackend → KqueueBackend
- [ ] Throw runtime_error if no backend available

### Task 3.4: Implement EventLoop::run()
**File:** `src/net/event_loop.cpp`
**Dependencies:** Task 3.3
**Tasks:**
- [ ] Call backend_->start()
- [ ] Return true on success, false on failure

### Task 3.5: Implement EventLoop::stop()
**File:** `src/net/event_loop.cpp`
**Dependencies:** Task 3.4
**Tasks:**
- [ ] Call backend_->stop()
- [ ] Clean up timer state

---

## Phase 4: Update CMakeLists.txt

### Task 4.1: Add New Source Files
**File:** `CMakeLists.txt` or `src/CMakeLists.txt`
**Dependencies:** Phase 1, Phase 2
**Tasks:**
- [ ] Add `src/net/epoll_backend.cpp` to hpactor_lib sources
- [ ] Add `src/net/kqueue_backend.cpp` to hpactor_lib sources

---

## Phase 5: Update Call Sites

### Task 5.1: Find EventLoop Construction Sites
**Dependencies:** Task 0.2, Task 3.4
**Tasks:**
- [ ] For each site that constructs EventLoop and immediately uses it, add `event_loop->run()` call after construction

### Task 5.2: Update EventLoop Construction in Tests
**Files:** `tests/net/test_event_loop.cpp` and other test files
**Dependencies:** Task 5.1
**Tasks:**
- [ ] Add `event_loop->run()` after construction in all test files
- [ ] Add `event_loop->stop()` in test cleanup

---

## Phase 6: Build & Test

### Task 6.1: Build Verification
**Command:** `cmake -S . -B build -GNinja && ninja -C build`
**Dependencies:** Phase 4
**Expected:** Clean build with no errors

### Task 6.2: Run All Tests
**Command:** `ctest --output-on-failure` (in build directory)
**Dependencies:** Task 6.1
**Expected:** All tests pass

---

## Dependency Graph

```
Phase 0 ──┬── Task 0.1 ── Task 0.2
          │
Phase 1 ──┬── Task 1.1 ── Task 1.2 ── Task 1.3 ── Task 1.4 ── Task 1.5 ── Task 1.6
          │
Phase 2 ──┬── Task 2.1 ── Task 2.2 (parallel to Phase 1)
          │
Phase 3 ──┬── Task 3.1 ── Task 3.2 ── Task 3.3 ── Task 3.4 ── Task 3.5
          │
Phase 4 ─┬── Task 4.1
          │
Phase 5 ─┬── Task 5.1 ── Task 5.2
          │
Phase 6 ─┬── Task 6.1 ── Task 6.2
```

---

## File Summary

| File | Phase |
|------|-------|
| `include/hpactor/net/epoll_backend.hpp` | 1.1 |
| `src/net/epoll_backend.cpp` | 1.2-1.6 |
| `include/hpactor/net/kqueue_backend.hpp` | 2.1 |
| `src/net/kqueue_backend.cpp` | 2.2 |
| `include/hpactor/net/event_loop.hpp` | 3.1 |
| `src/net/event_loop.cpp` | 3.2-3.5 |
| `CMakeLists.txt` or `src/CMakeLists.txt` | 4.1 |
| `tests/net/test_event_loop.cpp` (and others) | 5.1-5.2 |

---

## Open Questions (to resolve before Phase 3)

1. Should `run()` block indefinitely or return immediately?
   - Recommendation: Return immediately, caller runs event loop separately
2. Should we add a `EventLoop::backend_name()` for debugging?
   - Recommendation: Yes, add `std::string backend_name()` method
3. Should async operations in epoll/kqueue use a thread pool or return errors?
   - Recommendation: Use a simple thread pool for async send/recv for now

---

## Verification Checklist

After implementation, verify:
- [ ] EventLoop constructs without starting
- [ ] `run()` starts the backend
- [ ] `stop()` stops the backend cleanly
- [ ] Linux falls back to epoll if io_uring unavailable
- [ ] macOS falls back to kqueue if GCD unavailable
- [ ] All existing tests pass with new EventLoop lifecycle
- [ ] Timer functionality works with both backends
- [ ] FD registration/update/remove works with both backends
