# EventLoop Backend Fallback Design

**Date:** 2026-04-14
**Owner:** SKG7ON
**Status:** Draft
**Type:** Feature Implementation

## Overview

Refactor the EventLoop backend selection to provide fallback mechanisms for older kernels and add explicit `run()` method. Currently the EventLoop automatically selects io_uring (Linux) or GCD (macOS) on construction. This design adds kqueue (macOS) and epoll (Linux) as fallbacks when the preferred backend cannot be used.

---

## Current Implementation

### Backend Selection (event_loop.cpp:108-121)
```cpp
EventLoop::EventLoop() {
#if defined(__APPLE__)
    auto gcd_backend = std::make_unique<GcdBackend>();
    // ...
#elif defined(__linux__)
    auto iouring_backend = std::make_unique<IoUringBackend>();
    // ...
#else
    #error "Unsupported platform"
#endif
}
```

### Issues with Current Design

1. **No fallback**: If io_uring/GCD fails or is unsupported, the system has no alternative
2. **Auto-start on construction**: Backend starts immediately in constructor, no explicit control
3. **No feature detection**: No way to check if io_uring features (like provided buffers) are supported

---

## Proposed Implementation

### Backend Priority

**Linux:**
1. io_uring (with feature detection for provided buffers)
2. epoll (fallback)

**macOS:**
1. libdispatch/GCD
2. kqueue (fallback)

### Feature Detection

**io_uring Detection:**
- Check kernel version >= 5.1 for basic io_uring
- Check for `IORING_FEAT_NATIVE_WORKQUEUE` flag for provided buffers support
- Use `syscall(__NR_io_uring_enter, ...)` probe or check `/proc/sys/kernel/io_uring_max_setup` exists

**GCD Detection:**
- Always available on macOS (it's the native mechanism)
- Fallback needed only for older macOS or special environments

**kqueue Detection:**
- Available on all BSD/macOS
- Use `kqueue()` syscall - if it fails, the system doesn't support it

**epoll Detection:**
- Available on Linux 2.6+
- Use `epoll_create1()` syscall - if it fails, the system doesn't support it

### New Backend Classes

#### EpollBackend (`epoll_backend.hpp`, `epoll_backend.cpp`)

```cpp
class EpollBackend : public AsyncIoBackend {
public:
    EpollBackend();
    ~EpollBackend() override;

    bool start() override;
    void stop() override;

    bool add_fd(int fd, IoEvent events) override;
    bool update_fd(int fd, IoEvent events) override;
    bool remove_fd(int fd) override;

    // Fixed buffers not supported with epoll - return -1
    int register_buffer(const void* addr, size_t len) override;
    bool unregister_buffer(int buffer_id) override;

    // Use posix aio or send/recv for async operations
    void async_send(int fd, const iovec* bufs, int buf_count,
                    ActorId actor, uint32_t op_type) override;
    // ... other async operations ...

    uint64_t run_after(ActorId actor, int delay_ms) override;
    uint64_t run_every(ActorId actor, int interval_ms) override;
    void cancel_timer(uint64_t handle) override;

    int wait(int timeout_ms) override;
    void process_completions() override;

    void deliver_completion(OpCompletion completion) override;

private:
    int epoll_fd_ = -1;
    int timerfd_ = -1;

    // Timer management
    std::unordered_map<uint64_t, std::pair<uint64_t, ActorId>> timers_;  // handle -> (expires_at, actor)
    std::atomic<uint64_t> next_timer_handle_{1};

    // fd -> registered events for update tracking
    std::unordered_map<int, uint32_t> fd_events_;
};
```

#### KqueueBackend (`kqueue_backend.hpp`, `kqueue_backend.cpp`)

Similar structure to EpollBackend but using kevent.

### API Changes

#### EventLoop Constructor

Default constructor does NOT start the backend:

```cpp
EventLoop::EventLoop() {
    // Just create the backend adapter, don't start
#if defined(__APPLE__)
    auto gcd_backend = try_create<GcdBackend>();
    if (!gcd_backend) {
        gcd_backend = try_create<KqueueBackend>();
    }
#elif defined(__linux__)
    auto iouring_backend = try_create<IoUringBackend>();
    if (!iouring_backend) {
        iouring_backend = try_create<EpollBackend>();
    }
#endif
    backend_ = std::make_unique<BackendAdapter>(this, std::move(backend));
}
```

#### New EventLoop::run() Method

```cpp
class EventLoop {
public:
    // ... existing methods ...

    // Start the backend and begin processing events
    // Call this explicitly after construction
    bool run();

    // Stop the backend
    void stop();
};
```

### Backend Factory Pattern

```cpp
namespace {

template<typename Backend, typename... Args>
std::unique_ptr<AsyncIoBackend> try_create(Args&&... args) {
    try {
        auto backend = std::make_unique<Backend>(std::forward<Args>(args)...);
        if (backend->start()) {
            return backend;
        }
    } catch (...) {
        // Backend creation or start failed
    }
    return nullptr;
}

} // anonymous namespace
```

### Backend Selection Logic

```cpp
EventLoop::EventLoop() {
    std::unique_ptr<AsyncIoBackend> backend;

#if defined(__APPLE__)
    // Try GCD first (preferred on macOS)
    backend = try_create<GcdBackend>();
    if (!backend) {
        // Fall back to kqueue
        backend = try_create<KqueueBackend>();
    }
#elif defined(__linux__)
    // Try io_uring first (preferred on Linux)
    backend = try_create<IoUringBackend>();
    if (!backend) {
        // Fall back to epoll
        backend = try_create<EpollBackend>();
    }
#endif

    if (!backend) {
        throw std::runtime_error("No supported async I/O backend available");
    }

    backend_ = std::make_unique<BackendAdapter>(this, std::move(backend));
}
```

---

## File Changes

### New Files

| File | Purpose |
|------|---------|
| `include/hpactor/net/epoll_backend.hpp` | EpollBackend declaration |
| `src/net/epoll_backend.cpp` | EpollBackend implementation |
| `include/hpactor/net/kqueue_backend.hpp` | KqueueBackend declaration |
| `src/net/kqueue_backend.cpp` | KqueueBackend implementation |

### Modified Files

| File | Changes |
|------|---------|
| `include/hpactor/net/event_loop.hpp` | Add `run()` and `stop()` methods, update comments |
| `src/net/event_loop.cpp` | Implement backend factory, `run()`/`stop()`, update constructor |
| `CMakeLists.txt` | Add new source files to hpactor_lib |

---

## Implementation Details

### EpollBackend Structure

```cpp
// Uses epoll_ctl for fd registration
// Uses timerfd_create + EPOLLIN for timers
// Uses non-blocking posix aio or thread pool for async send/recv
// For connect: use non-blocking connect() + epoll monitoring
// For accept: use non-blocking accept() + epoll monitoring
```

### KqueueBackend Structure

```cpp
// Uses kevent for fd registration (EVFILT_READ, EVFILT_WRITE)
// Uses EVFILT_TIMER for timers
// Uses non-blocking posix aio or thread pool for async send/recv
```

### Thread Safety

- Backend implementations must be thread-safe for async operations
- The `BackendAdapter` already provides thread-safe routing to EventLoop
- Timer operations from different threads must be synchronized

### Backward Compatibility

- Existing code that constructs `EventLoop` and immediately uses it will break
- Fix: Call `event_loop->run()` after construction (or provide `run_and_wait()` convenience)

---

## Testing Considerations

1. **Unit Tests:**
   - Test feature detection functions
   - Test backend creation failures gracefully fall back

2. **Integration Tests:**
   - Test EventLoop works with each backend
   - Test timer functionality with each backend
   - Test fd registration/update/remove with each backend

3. **Platform Tests:**
   - Test on Linux with io_uring available
   - Test on Linux with only epoll (older kernel)
   - Test on macOS with GCD
   - Test on macOS with kqueue fallback (if possible)

---

## Open Questions

1. Should we add a `EventLoop::backend_name()` method for debugging/logging?
2. Should we expose the backend type via an enum for conditional behavior?
3. Should we add a CMake option to force a specific backend for testing?
4. How to handle provided buffers with epoll/kqueue (not supported - just return error)?

---

## Dependencies

- Linux: `<sys/epoll.h>`, `<sys/timerfd.h>`
- macOS: `<sys/event.h>`

No new external dependencies required.
