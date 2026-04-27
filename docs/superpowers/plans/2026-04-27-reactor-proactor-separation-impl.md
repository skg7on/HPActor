# Reactor/Proactor Separation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Separate the async I/O backends into pure Reactor (epoll/kqueue) and pure Proactor (io_uring/GCD) interfaces, parameterized at compile-time via CMake flag.

**Architecture:** Split `AsyncIoBackend` into `IProactorBackend` and `IReactorBackend`. Move backend implementations to `src/net/proactor/` and `src/net/reactor/` subdirs. EventLoop (Reactor only) wraps IReactorBackend. ActorSystem delegates to the appropriate backend via EventLoop.

**Tech Stack:** C++20, CMake, liburing (Linux), libdispatch (macOS), epoll/kqueue

---

## Key Architecture Notes

1. **ActorSystem does NOT directly own a backend** - it owns `EventLoop` (Reactor-only concept), which owns the backend.
2. **Proactor backends bypass EventLoop** - they deliver completions directly via a callback mechanism.
3. **IoEvent belongs to IReactorBackend**, not shared types.
4. **No `src/net/CMakeLists.txt`** - all sources are in root CMakeLists.txt.

---

## File Mapping

### Header Reorganization

| Old Path | New Path |
|----------|----------|
| `include/hpactor/net/async_io_backend.hpp` | **DELETE** |
| `include/hpactor/net/async_io_fwd.hpp` | **NEW** - shared OpType, OpCompletion only |
| `include/hpactor/net/proactor_backend.hpp` | **NEW** - IProactorBackend interface |
| `include/hpactor/net/reactor_backend.hpp` | **NEW** - IReactorBackend + IoEvent |
| `include/hpactor/net/iouring_backend.hpp` | `include/hpactor/net/proactor/iouring_backend.hpp` |
| `include/hpactor/net/gcd_backend.hpp` | `include/hpactor/net/proactor/gcd_backend.hpp` |
| `include/hpactor/net/epoll_backend.hpp` | `include/hpactor/net/reactor/epoll_backend.hpp` |
| `include/hpactor/net/kqueue_backend.hpp` | `include/hpactor/net/reactor/kqueue_backend.hpp` |
| `include/hpactor/net/proactor_dispatcher.hpp` | **NEW** - ProactorDispatcher |
| `include/hpactor/net/reactor_dispatcher.hpp` | **NEW** - ReactorDispatcher |

### Source Reorganization

| Old Path | New Path |
|----------|----------|
| `src/net/iouring_backend.cpp` | `src/net/proactor/iouring_backend.cpp` |
| `src/net/gcd_backend.cpp` | `src/net/proactor/gcd_backend.cpp` |
| `src/net/epoll_backend.cpp` | `src/net/reactor/epoll_backend.cpp` |
| `src/net/kqueue_backend.cpp` | `src/net/reactor/kqueue_backend.cpp` |
| `src/net/dispatcher.cpp` | **NEW** - dispatcher implementations |

---

## Task 1: Create shared types header (OpType, OpCompletion only)

**Files:**
- Create: `include/hpactor/net/async_io_fwd.hpp`

- [ ] **Step 1: Create async_io_fwd.hpp with OpType and OpCompletion only**

```cpp
// Copyright 2026 HPActor Contributors
#pragma once

#include <hpactor/types/types.hpp>
#include <sys/socket.h>
#include <sys/uio.h>

namespace hpactor {
namespace net {

enum class OpType : uint32_t {
    Send = 1,
    Recv = 2,
    Accept = 3,
    Connect = 4,
    TimerFired = 5,
    RecvFrom = 6,
    SendTo = 7,
};

struct OpCompletion {
    ActorId actor;
    OpType type;
    int fd;
    int result;
    uint64_t user_data = 0;
    sockaddr_in src_addr = {};
    socklen_t src_addr_len = 0;
};

} // namespace net
} // namespace hpactor
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/net/async_io_fwd.hpp
git commit -m "feat(net): add shared OpType and OpCompletion types"
```

---

## Task 2: Create IProactorBackend interface

**Files:**
- Create: `include/hpactor/net/proactor_backend.hpp`

- [ ] **Step 1: Create proactor_backend.hpp**

```cpp
// Copyright 2026 HPActor Contributors
#pragma once

#include <hpactor/net/async_io_fwd.hpp>

namespace hpactor {
namespace net {

class IProactorBackend {
public:
    virtual ~IProactorBackend() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual void async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;
    virtual void async_accept(int fd, ActorId actor) = 0;
    virtual void async_connect(int fd, const sockaddr* addr,
                               socklen_t addrlen, ActorId actor) = 0;

    virtual void async_send_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) = 0;
    virtual void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) = 0;
    virtual void async_sendto(int fd, const iovec* bufs, int buf_count,
                               const sockaddr* addr, socklen_t addrlen,
                               ActorId actor, uint32_t op_type) = 0;

    virtual uint64_t run_after(ActorId actor, int delay_ms) = 0;
    virtual uint64_t run_every(ActorId actor, int interval_ms) = 0;
    virtual void cancel_timer(uint64_t handle) = 0;

    virtual int wait(int timeout_ms) = 0;
    virtual void process_completions() = 0;

    virtual void deliver_completion(OpCompletion completion) = 0;
};

} // namespace net
} // namespace hpactor
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/net/proactor_backend.hpp
git commit -m "feat(net): add IProactorBackend interface"
```

---

## Task 3: Create IReactorBackend interface (includes IoEvent)

**Files:**
- Create: `include/hpactor/net/reactor_backend.hpp`

- [ ] **Step 1: Create reactor_backend.hpp with IoEvent defined here**

```cpp
// Copyright 2026 HPActor Contributors
#pragma once

#include <hpactor/net/async_io_fwd.hpp>

namespace hpactor {
namespace net {

enum class IoEvent : uint32_t {
    Read = 1 << 0,
    Write = 1 << 1,
};

class IReactorBackend {
public:
    virtual ~IReactorBackend() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual bool add_fd(int fd, IoEvent events) = 0;
    virtual bool update_fd(int fd, IoEvent events) = 0;
    virtual bool remove_fd(int fd) = 0;

    virtual int wait(int timeout_ms) = 0;
    virtual void process_events() = 0;

    virtual int register_buffer(const void* addr, size_t len) = 0;
    virtual bool unregister_buffer(int buffer_id) = 0;
};

} // namespace net
} // namespace hpactor
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/net/reactor_backend.hpp
git commit -m "feat(net): add IReactorBackend interface with IoEvent"
```

---

## Task 4: Update EventLoop to use IReactorBackend

**Files:**
- Modify: `include/hpactor/net/event_loop.hpp` - use `IReactorBackend*` instead of `AsyncIoBackend*`
- Modify: `src/net/event_loop.cpp` - update accordingly
- Note: This must happen BEFORE removing `set_read_handler` from backends

- [ ] **Step 1: Find EventLoop's backend member and update to IReactorBackend**

Current EventLoop has:
```cpp
std::unique_ptr<AsyncIoBackend> backend_;
```

Change to:
```cpp
std::unique_ptr<IReactorBackend> backend_;
```

- [ ] **Step 2: Update EventLoop::set_read_handler**

The `set_read_handler` method currently delegates to `backend_->set_read_handler()`. This must be refactored to be a no-op or removed entirely, since Reactor mode uses fd_actors_ map instead.

- [ ] **Step 3: Update EventLoop::wait() to call backend_->wait()**

- [ ] **Step 4: Update EventLoop::process_completions() if needed**

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor(net): update EventLoop to use IReactorBackend"
```

---

## Task 5: Create proactor/ directory and move IoUringBackend

**Files:**
- Create: `src/net/proactor/` and `include/hpactor/net/proactor/` directories
- Move: `src/net/iouring_backend.cpp` → `src/net/proactor/iouring_backend.cpp`
- Move: `include/hpactor/net/iouring_backend.hpp` → `include/hpactor/net/proactor/iouring_backend.hpp`

- [ ] **Step 1: Create directories and move files**

```bash
mkdir -p src/net/proactor include/hpactor/net/proactor
mv src/net/iouring_backend.cpp src/net/proactor/
mv include/hpactor/net/iouring_backend.hpp include/hpactor/net/proactor/
```

- [ ] **Step 2: Update #include paths in iouring_backend.cpp**

Change:
```cpp
#include <hpactor/net/iouring_backend.hpp>
```
To:
```cpp
#include <hpactor/net/proactor/iouring_backend.hpp>
```

And update `async_io_backend.hpp` reference to `async_io_fwd.hpp`:
```cpp
#include <hpactor/net/async_io_fwd.hpp>
```

- [ ] **Step 3: Update #include in iouring_backend.hpp**

Change `AsyncIoBackend` base class include to:
```cpp
#include <hpactor/net/proactor_backend.hpp>
```

- [ ] **Step 4: Update IoUringBackend class to inherit IProactorBackend**

In header, change:
```cpp
class IoUringBackend : public AsyncIoBackend {
```
To:
```cpp
class IoUringBackend : public IProactorBackend {
```

- [ ] **Step 5: Update CMakeLists.txt sources**

Change line ~204 in root CMakeLists.txt:
```cmake
target_sources(hpactor_lib PRIVATE src/net/proactor/iouring_backend.cpp)
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(net): move IoUringBackend to proactor/ subdirectory"
```

---

## Task 6: Create reactor/ directory and move EpollBackend

**Files:**
- Move: `src/net/epoll_backend.cpp` → `src/net/reactor/epoll_backend.cpp`
- Move: `include/hpactor/net/epoll_backend.hpp` → `include/hpactor/net/reactor/epoll_backend.hpp`

- [ ] **Step 1: Create directories and move files**

```bash
mkdir -p src/net/reactor include/hpactor/net/reactor
mv src/net/epoll_backend.cpp src/net/reactor/
mv include/hpactor/net/epoll_backend.hpp include/hpactor/net/reactor/
```

- [ ] **Step 2: Update #include paths in epoll_backend.cpp**

```cpp
#include <hpactor/net/reactor/epoll_backend.hpp>
#include <hpactor/net/reactor_backend.hpp>
```

- [ ] **Step 3: Update #include in epoll_backend.hpp**

Change `AsyncIoBackend` base class include to:
```cpp
#include <hpactor/net/reactor_backend.hpp>
```

- [ ] **Step 4: Update EpollBackend class to inherit IReactorBackend**

- [ ] **Step 5: Update CMakeLists.txt sources**

Change line ~204:
```cmake
target_sources(hpactor_lib PRIVATE src/net/reactor/epoll_backend.cpp)
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(net): move EpollBackend to reactor/ subdirectory"
```

---

## Task 7: Create reactor/ directory and move KqueueBackend

**Files:**
- Move: `src/net/kqueue_backend.cpp` → `src/net/reactor/kqueue_backend.cpp`
- Move: `include/hpactor/net/kqueue_backend.hpp` → `include/hpactor/net/reactor/kqueue_backend.hpp`

- [ ] **Step 1-5: Same pattern as Task 6 for KqueueBackend**

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(net): move KqueueBackend to reactor/ subdirectory"
```

---

## Task 8: Create proactor/ directory and move GcdBackend

**Files:**
- Move: `src/net/gcd_backend.cpp` → `src/net/proactor/gcd_backend.cpp`
- Move: `include/hpactor/net/gcd_backend.hpp` → `include/hpactor/net/proactor/gcd_backend.hpp`

- [ ] **Step 1-5: Same pattern as Task 5 for GcdBackend**

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(net): move GcdBackend to proactor/ subdirectory"
```

---

## Task 9: Delete old async_io_backend.hpp

**Files:**
- Delete: `include/hpactor/net/async_io_backend.hpp`

- [ ] **Step 1: Verify no remaining references**

```bash
grep -r "async_io_backend.hpp" include/ src/ --include="*.cpp" --include="*.hpp"
```
Should return no results.

- [ ] **Step 2: Delete the file**

```bash
rm include/hpactor/net/async_io_backend.hpp
```

- [ ] **Step 3: Commit**

```bash
git rm include/hpactor/net/async_io_backend.hpp
git commit -m "refactor(net): remove old AsyncIoBackend interface"
```

---

## Task 10: Create Dispatcher classes

**Files:**
- Create: `include/hpactor/net/proactor_dispatcher.hpp`
- Create: `include/hpactor/net/reactor_dispatcher.hpp`
- Create: `src/net/dispatcher.cpp`

### ProactorDispatcher

```cpp
// proactor_dispatcher.hpp
class ProactorDispatcher {
    void on_completion(OpCompletion completion) {
        // Route completion to appropriate handler
        switch (completion.type) {
            case OpType::TimerFired:
                // Deliver to timer system
                break;
            case OpType::Send:
            case OpType::Recv:
            case OpType::Accept:
            case OpType::Connect:
            case OpType::RecvFrom:
            case OpType::SendTo:
                // Deliver to actor mailbox
                break;
        }
    }

    void set_actor_system(ActorSystem* system) { system_ = system; }
    ActorSystem* system_ = nullptr;
};
```

### ReactorDispatcher

```cpp
// reactor_dispatcher.hpp
class ReactorDispatcher {
    // Map fd -> actor for readiness routing
    std::unordered_map<int, ActorId> fd_to_actor_;

    void on_readiness(int fd, IoEvent events) {
        ActorId actor = fd_to_actor_[fd];
        // Issue sync I/O, deliver to actor mailbox
    }

    void register_fd(int fd, ActorId actor) {
        fd_to_actor_[fd] = actor;
    }

    void unregister_fd(int fd) {
        fd_to_actor_.erase(fd);
    }
};
```

- [ ] **Step 1: Create proactor_dispatcher.hpp**

- [ ] **Step 2: Create reactor_dispatcher.hpp**

- [ ] **Step 3: Create dispatcher.cpp with implementations**

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat(net): add ProactorDispatcher and ReactorDispatcher"
```

---

## Task 11: Add CMake ENABLE_PROACTOR option

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add ENABLE_PROACTOR option**

Add after line 24:
```cmake
option(ENABLE_PROACTOR "Use Proactor I/O model (io_uring/GCD)" ON)
```

- [ ] **Step 2: Update backend selection logic**

Replace lines 197-220 with:

```cmake
if(ENABLE_PROACTOR)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(liburing_FOUND)
            message(STATUS "Using io_uring (Proactor)")
            target_sources(hpactor_lib PRIVATE src/net/proactor/iouring_backend.cpp)
        endif()
        # Always include epoll as fallback
        target_sources(hpactor_lib PRIVATE src/net/reactor/epoll_backend.cpp)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(STATUS "Using GCD (Proactor)")
        target_sources(hpactor_lib PRIVATE src/net/proactor/gcd_backend.cpp)
        # Always include kqueue as fallback
        target_sources(hpactor_lib PRIVATE src/net/reactor/kqueue_backend.cpp)
    endif()
else()
    # Reactor mode
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(STATUS "Using epoll (Reactor)")
        target_sources(hpactor_lib PRIVATE src/net/reactor/epoll_backend.cpp)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(STATUS "Using kqueue (Reactor)")
        target_sources(hpactor_lib PRIVATE src/net/reactor/kqueue_backend.cpp)
    endif()
endif()
```

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add ENABLE_PROACTOR CMake option"
```

---

## Task 12: Update ActorSystem EventLoop usage for policy

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

**Architecture Note:** ActorSystem owns `EventLoop` (Reactor-only). For Proactor mode, EventLoop wraps an IReactorBackend but Proactor backends deliver completions directly via callbacks, not through EventLoop.

- [ ] **Step 1: Add policy tag types**

Add at top of `actor_system.hpp`:
```cpp
struct ProactorPolicy {};
struct ReactorPolicy {};
```

- [ ] **Step 2: Add ENABLE_PROACTOR conditional for EventLoop backend type**

Current ActorSystem has:
```cpp
std::unique_ptr<net::EventLoop> network_loop_;
```

For Proactor mode, EventLoop may wrap a null/placeholder backend since Proactor delivers completions directly.

- [ ] **Step 3: Update network_thread_ initialization based on policy**

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "refactor: make ActorSystem work with both Proactor and Reactor modes"
```

---

## Task 13: Run tests and verify

**Files:**
- Build and test all 51 tests

- [ ] **Step 1: Configure and build**

```bash
cmake -S . -B build -GNinja
ninja -C build
```

- [ ] **Step 2: Run all tests**

```bash
ctest --output-on-failure
```

Expected: All 51 tests pass

- [ ] **Step 3: Test both modes**

```bash
# Test Proactor mode (default)
cmake -DENABLE_PROACTOR=ON -S . -B build-proactor -GNinja
ninja -C build-proactor
ctest --output-on-failure -C build-proactor

# Test Reactor mode
cmake -DENABLE_PROACTOR=OFF -S . -B build-reactor -GNinja
ninja -C build-reactor
ctest --output-on-failure -C build-reactor
```

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "test: verify all tests pass with both Proactor and Reactor modes"
```

---

## Task 14: Final verification

- [ ] **Step 1: Verify file structure**

```bash
ls -la include/hpactor/net/proactor/
ls -la include/hpactor/net/reactor/
ls -la src/net/proactor/
ls -la src/net/reactor/
```

- [ ] **Step 2: Verify no old references remain**

```bash
grep -r "async_io_backend.hpp" --include="*.cpp" --include="*.hpp"
grep -r "AsyncIoBackend" --include="*.cpp" --include="*.hpp"
```
Should return empty.

- [ ] **Step 3: Final commit**

```bash
git status
git commit -m "refactor(net): complete Reactor/Proactor separation"
```

---

## Summary of Commits

1. `feat(net): add shared OpType and OpCompletion types`
2. `feat(net): add IProactorBackend interface`
3. `feat(net): add IReactorBackend interface with IoEvent`
4. `refactor(net): update EventLoop to use IReactorBackend`
5. `refactor(net): move IoUringBackend to proactor/ subdirectory`
6. `refactor(net): move EpollBackend to reactor/ subdirectory`
7. `refactor(net): move KqueueBackend to reactor/ subdirectory`
8. `refactor(net): move GcdBackend to proactor/ subdirectory`
9. `refactor(net): remove old AsyncIoBackend interface`
10. `feat(net): add ProactorDispatcher and ReactorDispatcher`
11. `build: add ENABLE_PROACTOR CMake option`
12. `refactor: make ActorSystem work with both Proactor and Reactor modes`
13. `test: verify all tests pass with both Proactor and Reactor modes`
14. `refactor(net): complete Reactor/Proactor separation`
