# Async I/O Backend Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace epoll/kqueue with io_uring (Linux) and libdispatch (macOS) as the async I/O backend for HPActor's networking stack. All async operations complete via actor mailbox messages.

**Architecture:** An `AsyncIoBackend` abstract interface sits between upper layers (TcpTransport, TlsConnection, Registrar) and platform-specific implementations. `IoUringBackend` uses io_uring's proactive SQPOLL mode with fixed buffers and linked SQEs. `GcdBackend` uses libdispatch async socket operations on macOS. `EventLoop` delegates all I/O to the backend.

**Tech Stack:** C++20, liburing (Linux), libdispatch (macOS), OpenSSL already present. No exceptions, no RTTI.

**Reference:** `docs/superpowers/specs/2026-04-12-async-io-uring-migration-design.md`

---

## File Inventory

### New files to create

| File | Purpose |
|------|---------|
| `include/hpactor/net/async_io_backend.hpp` | Abstract interface, `OpType` enum, `IoEvent` enum |
| `include/hpactor/net/iouring_backend.hpp` | `IoUringBackend` class declaration |
| `include/hpactor/net/gcd_backend.hpp` | `GcdBackend` class declaration |
| `src/net/iouring_backend.cpp` | io_uring implementation |
| `src/net/gcd_backend.cpp` | libdispatch implementation |
| `tests/net/test_async_io_backend.cpp` | Interface + OpType unit tests |
| `tests/net/test_iouring_backend.cpp` | io_uring backend tests (Linux-only, skipped on macOS) |

### Existing files to modify

| File | Change |
|------|--------|
| `include/hpactor/net/event_loop.hpp` | Replace kqueue/epoll-specific declarations with delegation to `AsyncIoBackend` |
| `src/net/event_loop.cpp` | Refactor to own an `AsyncIoBackend` instance and delegate all operations |
| `src/net/tcp_transport.cpp` | Create platform-specific `AsyncIoBackend` instance |
| `src/net/tls_connection.cpp` | `send_raw()` calls `async_send_fixed()`, wire `on_fd_readable/writable` |
| `src/net/registrar_server.cpp` | Remove blocking `accept_loop()` thread, use `async_accept()` + EventLoop |
| `src/net/registrar_client.cpp` | Remove blocking connection/heartbeat threads, use EventLoop-driven async I/O |
| `CMakeLists.txt` | Add liburing (Linux) and libdispatch (macOS) dependencies |

---

## Phase 1: AsyncIoBackend Interface

### Files:
- Create: `include/hpactor/net/async_io_backend.hpp`
- Create: `tests/net/test_async_io_backend.cpp`

### Dependencies: None (pure interface)

---

### Task 1: Create `async_io_backend.hpp`

**Files:**
- Create: `include/hpactor/net/async_io_backend.hpp`

**`OpType` enum values:**
```cpp
enum class OpType : uint32_t {
    Send = 1,
    Recv = 2,
    Accept = 3,
    Connect = 4,
    TimerFired = 5,
    RecvFrom = 6,
    SendTo = 7,
};
```

**`IoEvent` enum values:**
```cpp
enum class IoEvent : uint32_t {
    Read  = 1 << 0,
    Write = 1 << 1,
};
```

**`OpCompletion` struct** — encodes the data delivered to an actor's mailbox:
```cpp
struct OpCompletion {
    ActorId actor;
    OpType  type;
    int     fd;         // fd the operation was on
    int     result;      // >= 0 bytes on success, < 0 errno on failure
    uint64_t user_data;  // original user_data from the SQE
};
```

**`user_data` encoding for io_uring**: Since io_uring CQEs carry only a `user_data` field, the backend must encode all routing information into it. The encoding packs three values into a single `uint64_t`:
- Bits 0–31: `fd` (the socket fd)
- Bits 32–63: `ActorId::value()` (the actor ID)
- Bits 56–63: `op_type` (8 bits sufficient for OpType enum)

```cpp
static uint64_t encode_user_data(int fd, ActorId actor, uint32_t op_type) {
    return (static_cast<uint64_t>(fd) & 0xFFFFFFFFULL) |
           ((actor.value() & 0xFFFFFFFFULL) << 32) |
           ((static_cast<uint64_t>(op_type) & 0xFFULL) << 56);
}

static void decode_user_data(uint64_t ud, int& fd, ActorId& actor, uint32_t& op_type) {
    fd = static_cast<int>(ud & 0xFFFFFFFFULL);
    actor = ActorId((ud >> 32) & 0xFFFFFFFFULL);
    op_type = static_cast<uint32_t>((ud >> 56) & 0xFFULL);
}
```

**`AsyncIoBackend` class** — full interface from design spec (Section 3), reproduced here for implementation reference:
```cpp
class AsyncIoBackend {
public:
    virtual ~AsyncIoBackend() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual bool add_fd(int fd, IoEvent events) = 0;
    virtual bool update_fd(int fd, IoEvent events) = 0;
    virtual bool remove_fd(int fd) = 0;

    // Buffer registration
    virtual int register_buffer(const void* addr, size_t len) = 0;
    virtual bool unregister_buffer(int buffer_id) = 0;

    // Vectored async I/O
    virtual void async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;

    // Fixed-buffer async I/O
    virtual void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                   ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                   ActorId actor, uint32_t op_type) = 0;

    // Connection operations
    virtual void async_accept(int fd, ActorId actor) = 0;
    virtual void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                               ActorId actor) = 0;

    // UDP operations
    virtual void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                 ActorId actor, uint32_t op_type) = 0;
    virtual void async_sendto(int fd, const iovec* bufs, int buf_count,
                               const sockaddr* addr, socklen_t addrlen,
                               ActorId actor, uint32_t op_type) = 0;

    // Timer operations
    virtual uint64_t run_after(ActorId actor, int delay_ms) = 0;
    virtual uint64_t run_every(ActorId actor, int interval_ms) = 0;
    virtual void cancel_timer(uint64_t handle) = 0;

    // Event loop pump
    virtual int wait(int timeout_ms) = 0;
    virtual void process_completions() = 0;
};
```

**NOTE:** Include `<sys/uio.h>` for `iovec`.

---

### Task 2: Create `tests/net/test_async_io_backend.cpp`

**Files:**
- Create: `tests/net/test_async_io_backend.cpp`

Write a simple unit test that:
- Verifies `OpType` enum values
- Verifies `IoEvent` enum values
- Verifies `IoEvent` flags can be combined with `|` and tested with `&`
- Verifies `OpCompletion` struct fields

Tests follow the existing pattern: assert-based, returns 0 on success.

---

### Task 3: Commit Phase 1

```bash
git add include/hpactor/net/async_io_backend.hpp tests/net/test_async_io_backend.cpp
git commit -m "feat: add AsyncIoBackend interface and OpType/IoEvent enums"
```

---

## Phase 2: CMake Dependency Wiring

### Files:
- Modify: `CMakeLists.txt`

---

### Task 4: Add liburing and libdispatch to CMakeLists.txt

Add after the OpenSSL `find_package` block:

```cmake
# Async I/O backends
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

---

### Task 5: Verify CMake configuration works (no-op build check)

```bash
cmake -S . -B build -GNinja 2>&1 | head -30
```

Expected: configure succeeds with no errors about missing liburing or dispatch.

---

### Task 6: Commit Phase 2

```bash
git add CMakeLists.txt
git commit -m "build: add liburing (Linux) and libdispatch (macOS) dependencies"
```

---

## Phase 3: IoUringBackend (Linux Only)

### Files:
- Create: `include/hpactor/net/iouring_backend.hpp`
- Create: `src/net/iouring_backend.cpp`
- Create: `tests/net/test_iouring_backend.cpp`
- Modify: `CMakeLists.txt` (add `src/net/iouring_backend.cpp` to hpactor_lib)

### Prerequisites: Phase 1 + Phase 2

---

### Task 7: Create `iouring_backend.hpp`

**Files:**
- Create: `include/hpactor/net/iouring_backend.hpp`

```cpp
#pragma once

#include <hpactor/net/async_io_backend.hpp>

#if defined(__linux__)
#include <liburing.h>
#else
// Stub definition for macOS compilation safety
#endif

namespace hpactor {
namespace net {

class IoUringBackend : public AsyncIoBackend {
public:
    IoUringBackend();
    ~IoUringBackend() override;

    bool start() override;
    void stop() override;

    bool add_fd(int fd, IoEvent events) override;
    bool update_fd(int fd, IoEvent events) override;
    bool remove_fd(int fd) override;

    int register_buffer(const void* addr, size_t len) override;
    bool unregister_buffer(int buffer_id) override;

    void async_send(int fd, const iovec* bufs, int buf_count,
                    ActorId actor, uint32_t op_type) override;
    void async_recv(int fd, const iovec* bufs, int buf_count,
                    ActorId actor, uint32_t op_type) override;

    void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type) override;
    void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type) override;

    void async_accept(int fd, ActorId actor) override;
    void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                        ActorId actor) override;

    void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                         ActorId actor, uint32_t op_type) override;
    void async_sendto(int fd, const iovec* bufs, int buf_count,
                        const sockaddr* addr, socklen_t addrlen,
                        ActorId actor, uint32_t op_type) override;

    uint64_t run_after(ActorId actor, int delay_ms) override;
    uint64_t run_every(ActorId actor, int interval_ms) override;
    void cancel_timer(uint64_t handle) override;

    int wait(int timeout_ms) override;
    void process_completions() override;

private:
    // Encode actor + op_type into user_data
    static uint64_t encode_user_data(int fd, ActorId actor, uint32_t op_type);
    static void decode_user_data(uint64_t user_data, ActorId& actor, uint32_t& op_type);

    // Submit pending SQEs to kernel
    int submit();

    struct io_uring ring_;

    // File fd → registered index (for IORING_REGISTER_FILES)
    std::unordered_map<int, unsigned> registered_fds_;

    // Registered buffers: buffer_id → (addr, len)
    std::vector<std::pair<const void*, size_t>> registered_buffers_;

    // ActorId → timer handle (for cancellation)
    std::unordered_map<uint64_t, uint64_t> actor_timer_handles_;

    // Ops pending submission (not yet submitted to kernel)
    std::vector<struct io_uring_sqe*> pending_sqes_;

    bool running_ = false;
};

} // namespace net
} // namespace hpactor
```

---

### Task 8: Create `src/net/iouring_backend.cpp`

**Files:**
- Create: `src/net/iouring_backend.cpp`

Key implementation details:

**Setup** (`start()`):
```cpp
struct io_uring_params params = {};
params.flags |= IORING_SETUP_SQPOLL;   // Proactive kernel-side polling
params.flags |= IORING_SETUP_SQ_AFF;   // Pin SQ thread to CPU (optional)
// params sq_thread_idle = 1000;        // ms before SQ thread sleeps

int ret = io_uring_queue_init_params(256, &ring_, &params);
if (ret < 0) { return false; }
running_ = true;
```

**File registration** (`add_fd()`):
- Pre-register socket fds via `io_uring_register_files()` (adds to `registered_fds_` map)
- On Linux 5.6+ can use `IORING_REGISTER_FILES` to avoid O_NONBLOCK per fd
- If registration not supported, fall back to adding fd to the ring with `IOSQE_FIXED_FILE` not set

**Buffer registration** (`register_buffer()`):
- Call `io_uring_register_buffers()` with `IORING_REGISTER_BUFFERS`
- Store (addr, len) in `registered_buffers_` indexed by returned buffer_id
- If already registered buffers exist, use `io_uring_register_buffers_update()` (ring-3)

**`async_send_fixed()`**:
```cpp
// Look up the registered buffer's base address from buffer_id
auto [buf_addr, buf_len] = registered_buffers_[buffer_id];
uint64_t buf_offset = offset;  // offset within the registered buffer

struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
io_uring_prep_write_fixed(sqe, fd,
                           static_cast<const char*>(buf_addr) + buf_offset,
                           len,
                           0,         // file offset (0 for socket, irrelevant for fixed)
                           buffer_id);
sqe->user_data = encode_user_data(fd, actor, op_type);
pending_sqes_.push_back(sqe);
```

**`async_recv_fixed()`**:
```cpp
auto [buf_addr, buf_len] = registered_buffers_[buffer_id];
uint64_t buf_offset = offset;  // offset within the registered buffer

struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
io_uring_prep_read_fixed(sqe, fd,
                          static_cast<char*>(const_cast<void*>(buf_addr)) + buf_offset,
                          len,
                          0,
                          buffer_id);
sqe->user_data = encode_user_data(fd, actor, op_type);
pending_sqes_.push_back(sqe);
```

**`async_accept()`**:
```cpp
struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
io_uring_prep_accept(sqe, fd, nullptr, nullptr, 0);
sqe->user_data = encode_user_data(fd, actor,
                                   static_cast<uint32_t>(OpType::Accept));
pending_sqes_.push_back(sqe);
```

**`async_connect()`**:
```cpp
struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
io_uring_prep_connect(sqe, fd, addr, addrlen);
sqe->user_data = encode_user_data(fd, actor,
                                   static_cast<uint32_t>(OpType::Connect));
pending_sqes_.push_back(sqe);
```

**Linked SQEs** — add `IOSQE_IO_LINK` flag to the SQE before the one that is linked:
```cpp
// Link sqe_a to sqe_b (sqe_a must complete before sqe_b starts)
sqe_a->flags |= IOSQE_IO_LINK;
```

**`submit()`**:
```cpp
if (!pending_sqes_.empty()) {
    int submitted = io_uring_submit(&ring_);
    if (submitted < 0) { abort(); } // submission failure = fatal
    pending_sqes_.clear();
}
```

**`wait()`**:
```cpp
struct io_uring_cqe* cqe = nullptr;
int ret = io_uring_wait_cqe(&ring_, &cqe);  // blocks until at least one CQE
if (ret < 0) { return ret; }
// Store cqe for process_completions() to drain
return ret;  // number of triggered events
```

**`process_completions()`**:
```cpp
struct io_uring_cqe* cqe = nullptr;
while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
    int fd;
    ActorId actor;
    uint32_t op_type;
    decode_user_data(cqe->user_data, fd, actor, op_type);

    // Build OpCompletion and deliver to actor's mailbox
    OpCompletion completion{
        .actor   = actor,
        .type    = static_cast<OpType>(op_type),
        .fd      = fd,
        .result  = cqe->res,
        .user_data = cqe->user_data,
    };
    deliver_to_actor(completion);  // posts to actor's mailbox

    io_uring_cqe_seen(&ring_, cqe);
}
```

**`run_after()` / `run_every()` using `IORING_OP_TIMEOUT`**:
```cpp
uint64_t run_after(ActorId actor, int delay_ms) override {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    struct __kernel_timespec ts;
    ts.tv_sec = delay_ms / 1000;
    ts.tv_nsec = (delay_ms % 1000) * 1000000;
    io_uring_prep_timeout(sqe, &ts, 0, 0);
    sqe->user_data = encode_user_data(actor, static_cast<uint32_t>(OpType::TimerFired));
    pending_sqes_.push_back(sqe);
    return next_timer_handle_++;
}
```

**Timer cancellation** (`cancel_timer()`): io_uring doesn't have direct timer cancellation. On cancel, set a flag that is checked when the timeout CQE fires — if cancelled, discard the CQE silently.

---

### Task 9: Write io_uring backend tests

**Files:**
- Create: `tests/net/test_iouring_backend.cpp`

**NOTE:** These tests only compile and run on Linux. Use preprocessor guard:
```cpp
#if !defined(__linux__)
int main() { return 0; }  // Skip on non-Linux
#else
// Full tests
#endif
```

Tests to write:
1. `IoUringBackend::start()` returns true and `running_` is set
2. `register_buffer()` returns valid buffer_id >= 0
3. `unregister_buffer()` returns true
4. `add_fd()` / `update_fd()` / `remove_fd()` basic sanity (create a pipe fd for testing)
5. Linked SQE: submit a write linked to a read, verify both complete
6. `run_after()` timer fires and delivers `OpType::TimerFired`
7. `async_accept()` on a listen socket submits an accept SQE
8. `async_connect()` submits a connect SQE

For socket tests, create a socketpair with `::socketpair(AF_UNIX, SOCK_STREAM, 0, fds)`.

---

### Task 10: Add iouring_backend.cpp to hpactor_lib

**Files:**
- Modify: `CMakeLists.txt`

Add `src/net/iouring_backend.cpp` to the `hpactor_lib` source list (same block as other `src/net/*.cpp` files). Wrap with `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")`.

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(hpactor_lib STATIC
        ...
        src/net/iouring_backend.cpp
    )
endif()
```

---

### Task 11: Build and verify

```bash
ninja -C build 2>&1 | tail -20
```

Expected: Links successfully on Linux. On macOS, `iouring_backend.cpp` is not compiled.

---

### Task 12: Commit Phase 3

```bash
git add include/hpactor/net/iouring_backend.hpp src/net/iouring_backend.cpp tests/net/test_iouring_backend.cpp CMakeLists.txt
git commit -m "feat: add IoUringBackend implementation for Linux"
```

---

## Phase 4: GcdBackend (macOS Only)

### Files:
- Create: `include/hpactor/net/gcd_backend.hpp`
- Create: `src/net/gcd_backend.cpp`
- Modify: `CMakeLists.txt`

### Prerequisites: Phase 1 + Phase 2

---

### Task 13: Create `gcd_backend.hpp`

**Files:**
- Create: `include/hpactor/net/gcd_backend.hpp`

```cpp
#pragma once

#include <hpactor/net/async_io_backend.hpp>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
// Stub for Linux compilation safety
#endif

namespace hpactor {
namespace net {

class GcdBackend : public AsyncIoBackend {
public:
    GcdBackend();
    ~GcdBackend() override;

    bool start() override;
    void stop() override;

    bool add_fd(int fd, IoEvent events) override;
    bool update_fd(int fd, IoEvent events) override;
    bool remove_fd(int fd) override;

    int register_buffer(const void* addr, size_t len) override;
    bool unregister_buffer(int buffer_id) override;

    void async_send(int fd, const iovec* bufs, int buf_count,
                    ActorId actor, uint32_t op_type) override;
    void async_recv(int fd, const iovec* bufs, int buf_count,
                    ActorId actor, uint32_t op_type) override;

    void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type) override;
    void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type) override;

    void async_accept(int fd, ActorId actor) override;
    void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                        ActorId actor) override;

    void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                         ActorId actor, uint32_t op_type) override;
    void async_sendto(int fd, const iovec* bufs, int buf_count,
                        const sockaddr* addr, socklen_t addrlen,
                        ActorId actor, uint32_t op_type) override;

    uint64_t run_after(ActorId actor, int delay_ms) override;
    uint64_t run_every(ActorId actor, int interval_ms) override;
    void cancel_timer(uint64_t handle) override;

    int wait(int timeout_ms) override;
    void process_completions() override;

private:
    // Convert IoEvent flags to dispatch_source flags
    dispatch_source_type_t ioevent_to_dispatch_source_type(IoEvent events);

    // Deliver completion to actor
    void deliver_completion(OpCompletion completion);

    dispatch_queue_t dispatch_queue_;   // main dispatch queue for completions

    // fd → dispatch_source_t (for read/write availability on listen fds)
    std::unordered_map<int, dispatch_source_t> fd_sources_;

    // fd → dispatch_source_t (for accept on listen fds)
    std::unordered_map<int, dispatch_source_t> accept_sources_;

    // Timer handle → dispatch_source_t
    std::unordered_map<uint64_t, dispatch_source_t> timer_sources_;
    std::atomic<uint64_t> next_timer_handle_{1};

    // ActorId → pending ops (for cancellation tracking)
    std::unordered_map<uint64_t, ActorId> pending_ops_;

    // Buffer storage for fixed buffers
    std::vector<std::pair<const void*, size_t>> registered_buffers_;

    bool running_ = false;
};

} // namespace net
} // namespace hpactor
```

---

### Task 14: Create `src/net/gcd_backend.cpp`

**Files:**
- Create: `src/net/gcd_backend.cpp`

**Key implementation notes:**

**Setup** (`start()`):
```cpp
dispatch_queue_ = dispatch_queue_create("com.hpactor.gcdbackend",
                                        DISPATCH_QUEUE_SERIAL);
running_ = true;
```

**`async_send()`** using `dispatch_write()` — must handle all `buf_count` entries:
```cpp
void GcdBackend::async_send(int fd, const iovec* bufs, int buf_count,
                             ActorId actor, uint32_t op_type) {
    // Concatenate all iovec buffers into one contiguous region
    // because dispatch_write() only accepts a single dispatch_data_t
    size_t total_len = 0;
    for (int i = 0; i < buf_count; ++i) {
        total_len += bufs[i].iov_len;
    }
    std::vector<uint8_t> contiguous(total_len);
    size_t offset = 0;
    for (int i = 0; i < buf_count; ++i) {
        std::memcpy(contiguous.data() + offset, bufs[i].iov_base, bufs[i].iov_len);
        offset += bufs[i].iov_len;
    }

    dispatch_data_t data = dispatch_data_create(
        contiguous.data(), contiguous.size(), dispatch_queue_,
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);

    // Retain the original buffer copy for the async operation
    auto* retained = new std::vector<uint8_t>(std::move(contiguous));
    dispatch_set_context(data, retained);
    dispatch_set_finalizer_f(data, [](void* ctx) {
        delete static_cast<std::vector<uint8_t>*>(ctx);
    });

    dispatch_write(fd, data, dispatch_queue_, ^(dispatch_data_t d, int err) {
        OpCompletion completion{
            .actor  = actor,
            .type   = static_cast<OpType>(op_type),
            .fd     = fd,
            .result = err < 0 ? err : static_cast<int>(dispatch_data_get_size(d)),
            .user_data = 0,
        };
        deliver_completion(completion);
    });
}
```

**`async_recv()`** using `dispatch_read()` — scatter received data across all `buf_count` entries:
```cpp
void GcdBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                              ActorId actor, uint32_t op_type) {
    // Estimate receive size as sum of all buffer lengths
    size_t recv_capacity = 0;
    for (int i = 0; i < buf_count; ++i) {
        recv_capacity += bufs[i].iov_len;
    }

    dispatch_data_t recv_data = dispatch_data_create(
        nullptr, recv_capacity, dispatch_queue_, DISPATCH_DATA_DESTRUCTOR_DEFAULT);

    dispatch_read(fd, recv_capacity, dispatch_queue_,
                  ^(dispatch_data_t d, int err) {
        size_t total_received = dispatch_data_get_size(d);
        size_t offset = 0;
        for (int i = 0; i < buf_count && offset < total_received; ++i) {
            size_t chunk = std::min(bufs[i].iov_len, total_received - offset);
            dispatch_data_apply(d, ^(dispatch_data_t region,
                                      size_t region_offset,
                                      const void* src,
                                      size_t len) {
                if (region_offset >= offset && region_offset < offset + chunk) {
                    std::memcpy(bufs[i].iov_base,
                                static_cast<const uint8_t*>(src) + (region_offset - offset),
                                chunk);
                }
                return (bool)true;
            });
            offset += chunk;
        }
        OpCompletion completion{
            .actor  = actor,
            .type   = static_cast<OpType>(op_type),
            .fd     = fd,
            .result = err < 0 ? err : static_cast<int>(total_received),
            .user_data = 0,
        };
        deliver_completion(completion);
    });
}
```

Note: `dispatch_data_apply` iterates over dispatch_data regions. The closure captures `offset` and `bufs` references — careful lifetime management needed. Alternatively, use `dispatch_data_create_subrange` to extract each region and copy it to the corresponding `iov_base`. Simpler approach: accumulate all received bytes in one contiguous buffer first, then copy to iovec entries.

**Important constraint**: The source buffers passed to `async_send()` must remain valid until the dispatch callback fires (at which point the retained copy is used). Callers must not modify or free send buffers while an async send is in-flight.

**`async_accept()`** using `dispatch_source`:
```cpp
void GcdBackend::async_accept(int fd, ActorId actor) {
    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_READ, fd, 0, dispatch_queue_);

    dispatch_source_set_event_handler(source, ^{
        int client_fd = ::accept(fd, nullptr, nullptr);
        if (client_fd >= 0) {
            OpCompletion completion{
                .actor  = actor,
                .type   = OpType::Accept,
                .fd     = client_fd,
                .result = client_fd,
                .user_data = 0,
            };
            deliver_completion(completion);
        }
    });

    dispatch_resume(source);
    accept_sources_[fd] = source;
}
```

**`run_after()`** using `dispatch_after()`:
```cpp
uint64_t GcdBackend::run_after(ActorId actor, int delay_ms) {
    uint64_t handle = next_timer_handle_++;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay_ms * 1000000LL),
                   dispatch_queue_, ^{
        if (cancelled_timers_.count(handle)) return;
        OpCompletion completion{
            .actor  = actor,
            .type   = OpType::TimerFired,
            .fd     = -1,
            .result = 0,
            .user_data = handle,
        };
        deliver_completion(completion);
    });
    return handle;
}
```

**`wait()`**: On macOS with libdispatch, there is no single "wait" call — completions are delivered via dispatch handlers. `wait()` should call `dispatch_sync` on the dispatch queue to process already-queued completions, or use `dispatch_wait()` pattern if needed. A reasonable implementation:
```cpp
int GcdBackend::wait(int timeout_ms) {
    // Process any pending dispatch events
    // Returns 0 (completions are delivered via callbacks, not here)
    return 0;
}
```
The actual "wait" is implicit in the dispatch queue model.

**`deliver_completion()`**: Posts the `OpCompletion` to the actor's mailbox. The actor system must provide a way to enqueue a message from outside the actor's thread context. This will require `ActorSystem::enqueue_completion(ActorId, OpCompletion)` — if this doesn't exist yet, it is a new interface that needs to be added. See Task 22.

---

### Task 15: Add gcd_backend.cpp to hpactor_lib

**Files:**
- Modify: `CMakeLists.txt`

Add `src/net/gcd_backend.cpp` under `elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")` block.

---

### Task 16: Build and verify

```bash
ninja -C build 2>&1 | tail -20
```

Expected: Links successfully on macOS.

---

### Task 17: Commit Phase 4

```bash
git add include/hpactor/net/gcd_backend.hpp src/net/gcd_backend.cpp CMakeLists.txt
git commit -m "feat: add GcdBackend implementation for macOS"
```

---

## Phase 5: EventLoop Refactor — Delegate to AsyncIoBackend

### Files:
- Modify: `include/hpactor/net/event_loop.hpp`
- Modify: `src/net/event_loop.cpp`

### Prerequisites: Phase 1 + Phase 3 + Phase 4

---

### Task 18: Refactor `event_loop.hpp`

**Files:**
- Modify: `include/hpactor/net/event_loop.hpp`

**Keep** (for backward compatibility with existing code that uses EventLoop directly):
- `EventLoop` class declaration
- `wait()`, `add_fd()`, `update_fd()`, `remove_fd()`, `run_after()`, `run_every()`, `cancel_timer()`, `system_fd()` — but these now delegate to `AsyncIoBackend`

**Add:**
- `#include <hpactor/net/async_io_backend.hpp>`
- A private `AsyncIoBackend* backend_` member (owned)
- A factory method to create the platform-specific backend:
  ```cpp
  static std::unique_ptr<AsyncIoBackend> create_backend();
  ```
- An `enqueue_completion(OpCompletion)` method for use by the backends

**Remove:**
- All kqueue/epoll-specific includes and code (no more `#if defined(__APPLE__)` blocks in this header)

The new `EventLoop` looks like:
```cpp
class EventLoop {
public:
    EventLoop(ActorSystem* actor_system = nullptr);
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool add_fd(int fd, IoEvent events);
    bool update_fd(int fd, IoEvent events);
    bool remove_fd(int fd);

    int register_buffer(const void* addr, size_t len);
    bool unregister_buffer(int buffer_id);

    int wait(int timeout_ms);

    // These delegate to backend_
    void async_send(int fd, const iovec* bufs, int buf_count,
                    ActorId actor, uint32_t op_type);
    void async_recv(int fd, const iovec* bufs, int buf_count,
                    ActorId actor, uint32_t op_type);
    void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                           ActorId actor, uint32_t op_type);
    void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                           ActorId actor, uint32_t op_type);
    void async_accept(int fd, ActorId actor);
    void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                        ActorId actor);
    void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                         ActorId actor, uint32_t op_type);
    void async_sendto(int fd, const iovec* bufs, int buf_count,
                        const sockaddr* addr, socklen_t addrlen,
                        ActorId actor, uint32_t op_type);

    uint64_t run_after(ActorId actor, int delay_ms);
    uint64_t run_every(ActorId actor, int interval_ms);
    void cancel_timer(uint64_t handle);

    int system_fd() const;  // returns backend_->wait() result or -1

    // For backends to enqueue completions
    void enqueue_completion(OpCompletion completion);

private:
    std::unique_ptr<AsyncIoBackend> backend_;
    ActorSystem* actor_system_ = nullptr;
};
```

---

### Task 19: Refactor `event_loop.cpp`

**Files:**
- Modify: `src/net/event_loop.cpp`

All existing kqueue/epoll code is replaced. The new implementation:
- `start()` / constructor: calls `create_backend()` and stores in `backend_`
- Each method simply forwards to `backend_->method()`
- `wait()` forwards to `backend_->wait()`, then calls `backend_->process_completions()`

```cpp
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/iouring_backend.hpp>
#include <hpactor/net/gcd_backend.hpp>

#if defined(__linux__)
#include <hpactor/net/iouring_backend.hpp>
#elif defined(__APPLE__)
#include <hpactor/net/gcd_backend.hpp>
#endif

namespace hpactor {
namespace net {

std::unique_ptr<AsyncIoBackend> EventLoop::create_backend() {
#if defined(__linux__)
    return std::make_unique<IoUringBackend>();
#elif defined(__APPLE__)
    return std::make_unique<GcdBackend>();
#else
    return nullptr;
#endif
}

EventLoop::EventLoop(ActorSystem* actor_system)
    : backend_(create_backend()),
      actor_system_(actor_system) {
    if (!backend_ || !backend_->start()) {
        // Log error but don't throw (no exceptions)
    }
}

EventLoop::~EventLoop() {
    if (backend_) {
        backend_->stop();
    }
}

bool EventLoop::add_fd(int fd, IoEvent events) {
    return backend_->add_fd(fd, events);
}

bool EventLoop::update_fd(int fd, IoEvent events) {
    return backend_->update_fd(fd, events);
}

bool EventLoop::remove_fd(int fd) {
    return backend_->remove_fd(fd);
}

int EventLoop::register_buffer(const void* addr, size_t len) {
    return backend_->register_buffer(addr, len);
}

bool EventLoop::unregister_buffer(int buffer_id) {
    return backend_->unregister_buffer(buffer_id);
}

int EventLoop::wait(int timeout_ms) {
    int n = backend_->wait(timeout_ms);
    backend_->process_completions();
    return n;
}

void EventLoop::async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) {
    backend_->async_send(fd, bufs, buf_count, actor, op_type);
}

void EventLoop::async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) {
    backend_->async_recv(fd, bufs, buf_count, actor, op_type);
}

void EventLoop::async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                  ActorId actor, uint32_t op_type) {
    backend_->async_send_fixed(fd, buffer_id, offset, len, actor, op_type);
}

void EventLoop::async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                  ActorId actor, uint32_t op_type) {
    backend_->async_recv_fixed(fd, buffer_id, offset, len, actor, op_type);
}

void EventLoop::async_accept(int fd, ActorId actor) {
    backend_->async_accept(fd, actor);
}

void EventLoop::async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                               ActorId actor) {
    backend_->async_connect(fd, addr, addrlen, actor);
}

void EventLoop::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) {
    backend_->async_recvfrom(fd, bufs, buf_count, actor, op_type);
}

void EventLoop::async_sendto(int fd, const iovec* bufs, int buf_count,
                              const sockaddr* addr, socklen_t addrlen,
                              ActorId actor, uint32_t op_type) {
    backend_->async_sendto(fd, bufs, buf_count, addr, addrlen, actor, op_type);
}

uint64_t EventLoop::run_after(ActorId actor, int delay_ms) {
    return backend_->run_after(actor, delay_ms);
}

uint64_t EventLoop::run_every(ActorId actor, int interval_ms) {
    return backend_->run_every(actor, interval_ms);
}

void EventLoop::cancel_timer(uint64_t handle) {
    backend_->cancel_timer(handle);
}

int EventLoop::system_fd() const {
    return -1;  // No longer applicable — backends handle their own fds
}

void EventLoop::enqueue_completion(OpCompletion completion) {
    // Post completion to actor's mailbox
    // This requires ActorSystem::enqueue() to exist
    // See Task 22
}

} // namespace net
} // namespace hpactor
```

---

### Task 20: Build and verify

```bash
ninja -C build 2>&1 | tail -30
```

Expected: Links successfully. `EventLoop::enqueue_completion()` is a stub at this point (Phase 5 only — it will be wired in Phase 5.5).

---

### Task 21: Commit Phase 5

```bash
git add include/hpactor/net/event_loop.hpp src/net/event_loop.cpp
git commit -m "refactor: EventLoop delegates to AsyncIoBackend"
```

---

## Phase 5.5: ActorSystem Completion Enqueue Interface

### Files:
- Modify: `include/hpactor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

### Prerequisites: Phase 5

---

### Task 21a: Add `ActorSystem::enqueue_completion` stub

**Files:**
- Modify: `include/hpactor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

The `GcdBackend` and `IoUringBackend` both need a thread-safe way to deliver `OpCompletion` to an actor's mailbox from any thread. Add this to `ActorSystem`:

```cpp
// In actor_system.hpp (public section):
// Enqueue an OpCompletion to an actor's mailbox from an external thread/context.
// Thread-safe: uses the same mailbox locking as normal message enqueue.
void enqueue_completion(ActorId actor, OpCompletion completion);
```

In `actor_system.cpp`, implement as:
```cpp
void ActorSystem::enqueue_completion(ActorId actor, OpCompletion completion) {
    // Look up actor by ID
    // Call actor's mailbox enqueue with the completion as a special message
    // If actor is local, enqueue directly to its mailbox
    // If actor is remote, route accordingly
}
```

This is a stub at this stage — it should compile and link but the actual routing to actor mailboxes is completed in Phase 6.

---

### Task 21b: Wire `EventLoop::enqueue_completion()` to `ActorSystem`

**Files:**
- Modify: `src/net/event_loop.cpp`

Now that `ActorSystem::enqueue_completion()` exists, update `EventLoop` to hold an `ActorSystem*` and call it:

```cpp
EventLoop::EventLoop(ActorSystem* actor_system)
    : backend_(create_backend()),
      actor_system_(actor_system) {
    if (!backend_ || !backend_->start()) {
        // ...
    }
}

void EventLoop::enqueue_completion(OpCompletion completion) {
    if (actor_system_) {
        actor_system_->enqueue_completion(completion.actor, completion);
    }
}
```

Update `TcpTransport` to pass `this` (or its `ActorSystem*`) to `EventLoop` constructor.

---

### Task 21c: Build and verify

```bash
ninja -C build 2>&1 | tail -20
```

---

### Task 21d: Commit Phase 5.5

```bash
git add include/hpactor/actor_system.hpp src/actor/actor_system.cpp src/net/event_loop.cpp src/net/tcp_transport.cpp
git commit -m "feat: add ActorSystem::enqueue_completion for async I/O completions"
```

---

## Phase 6: TcpTransport + TlsConnection Async Wiring

### Files:
- Modify: `src/net/tcp_transport.cpp`
- Modify: `src/net/tls_connection.cpp`
- Modify: `src/net/acceptor.cpp`

### Prerequisites: Phase 5.5

---

### Task 22: Update `TcpTransport` to pass ActorSystem to EventLoop

**Files:**
- Modify: `src/net/tcp_transport.cpp`
- Modify: `include/hpactor/net/tcp_transport.hpp` (if needed)

The `TcpTransport` constructor currently creates `EventLoop` on the stack. Update to pass the owning `ActorSystem` pointer to `EventLoop`.

```cpp
TcpTransport::TcpTransport(NodeId node_id,
                           const TlsConfig& tls_config,
                           const PoolConfig& pool_config,
                           NodeRegistry* registry,
                           ActorSystem* actor_system)
    : node_id_(node_id),
      loop_(actor_system),  // pass ActorSystem to EventLoop
      ...
```

---

### Task 23: Update `TlsConnection` to use async I/O

**Files:**
- Modify: `src/net/tls_connection.cpp`
- Modify: `include/hpactor/net/tls_connection.hpp`

**Changes to `TlsConnection`**:

1. Add `ActorId actor_id_` member — the actor ID this connection belongs to
2. Add `int send_buffer_id_` and `int recv_buffer_id_` — registered buffer IDs
3. Refactor `send_raw()` to use `async_send_fixed()`:
```cpp
void TlsConnection::send_raw(const bytes& data) {
    if (fd_ < 0) return;
    loop_->async_send_fixed(fd_, send_buffer_id_, 0, data.size(),
                            actor_id_, static_cast<uint32_t>(OpType::Send));
    // Don't block — actor continues or yields
}
```

4. Register a send and recv buffer on construction:
```cpp
// In constructor, after fd is set:
send_buffer_id_ = loop_->register_buffer(send_buffer_.data(), send_buffer_.size());
recv_buffer_id_ = loop_->register_buffer(recv_buffer_.data(), recv_buffer_.size());
```

5. Wire `on_fd_readable()` to actually call `loop_->async_recv_fixed()` when the EventLoop notifies the fd is readable. When the recv CQE fires, `handle_read()` is called with the received data.

6. Wire `on_fd_writable()` similarly for flush scenarios.

**Key insight**: The actor model integration means the `TlsConnection` is used by a "connection actor" that owns it. When an async send completes, the completion message goes to that actor's mailbox. The actor's `Behavior` handles the completion.

---

### Task 24: Wire `Acceptor` to use EventLoop's async_accept

**Files:**
- Modify: `src/net/acceptor.cpp`

The `Acceptor` already uses `EventLoop` for read events. The `handle_read()` method currently calls `accept()`. Update it to:
- Call `loop_->async_accept(listen_fd_, actor_id_)` instead of blocking `accept()`
- When the accept CQE fires, the completion goes to the `AcceptorActor`'s mailbox
- The `AcceptorActor`'s Behavior handles `OpCompletion` with `OpType::Accept`, extracts `client_fd`, and continues

---

### Task 25: Build and verify

```bash
ninja -C build 2>&1 | tail -40
```

Expected: Compiles. Common issues: missing `#include <sys/uio.h>`, `ActorId` not found in `async_io_backend.hpp` — add `#include <hpactor/types.hpp>`.

---

### Task 26: Commit Phase 6

```bash
git add src/net/tcp_transport.cpp src/net/tls_connection.cpp src/net/acceptor.cpp
git commit -m "feat: wire TlsConnection and Acceptor to async I/O backend"
```

---

## Phase 7: RegistrarServer Migration

### Files:
- Modify: `src/net/registrar_server.cpp`

### Prerequisites: Phase 6

---

### Task 27: Refactor `RegistrarServer` to use EventLoop

**Files:**
- Modify: `src/net/registrar_server.cpp`

**Before** (blocking thread):
```cpp
void RegistrarServer::accept_loop() {
    while (running_) {
        fd_set readfds;
        // select() on server fd + all client fds
        select(max_fd + 1, &readfds, nullptr, nullptr, nullptr);
        // handle_accept() for server fd
        // handle_read() for client fds
    }
}
```

**After**:
- The `RegistrarServer` becomes a `RegistrarServerActor` with a `Behavior`
- `accept_loop()` is eliminated — instead, on startup, call `loop_->async_accept(listen_fd_, registrar_actor_id_)`
- When the accept CQE fires, `OpCompletion` with `OpType::Accept` is delivered to `RegistrarServerActor`'s mailbox
- The actor's Behavior handles `OpCompletion(OpType::Accept, client_fd)`, registers the new client fd with the loop via `add_fd()`, and sets up async recv for incoming messages
- When recv CQE fires → `OpCompletion(OpType::Recv)` → actor handles it, parses message, dispatches to appropriate handler

**The `RegistrarServerActor` Behavior handlers:**
```cpp
Behavior handle_accept = [this](OpCompletion& op) {
    if (op.type != OpType::Accept) { return; }
    int client_fd = op.result;
    loop_->add_fd(client_fd, IoEvent::Read);
    loop_->async_recv(client_fd, recv_bufs, 1,
                      actor_id_, static_cast<uint32_t>(OpType::Recv));
};

Behavior handle_recv = [this](OpCompletion& op) {
    if (op.type != OpType::Recv) { return; }
    // Parse registrar protocol message
    handle_message(op.fd, op.result);
    // Re-arm recv
    loop_->async_recv(op.fd, recv_bufs, 1,
                      actor_id_, static_cast<uint32_t>(OpType::Recv));
};
```

**All blocking `select()` loops replaced** with EventLoop-driven I/O.

---

### Task 28: Build and verify

```bash
ninja -C build 2>&1 | tail -30
```

---

### Task 29: Commit Phase 7

```bash
git add src/net/registrar_server.cpp
git commit -m "feat: migrate RegistrarServer off blocking select thread"
```

---

## Phase 8: RegistrarClient Migration

### Files:
- Modify: `src/net/registrar_client.cpp`

### Prerequisites: Phase 7

---

### Task 30: Refactor `RegistrarClient` to use EventLoop

**Files:**
- Modify: `src/net/registrar_client.cpp`

**Before**:
```cpp
// Thread 1: connection_loop — select() on server TCP socket
// Thread 2: heartbeat_loop — sleep + send heartbeat
// Thread 3: resolve_node — UDP with select() timeout
```

**After**:
- `RegistrarClient` becomes `RegistrarClientActor`
- `connection_loop()` thread eliminated — replaced by EventLoop-driven `async_connect()` + `async_recv()`
- `heartbeat_loop()` thread eliminated — replaced by `loop_->run_every(actor_id_, heartbeat_interval_ms)` timer
- `resolve_node()` UDP still uses `async_recvfrom()` / `async_sendto()`

**Actor Behavior handlers:**
```cpp
Behavior handle_connect = [this](OpCompletion& op) {
    if (op.type != OpType::Connect) { return; }
    if (op.result < 0) {
        schedule_reconnect();
        return;
    }
    loop_->async_recv(server_fd_, recv_bufs, 1,
                      actor_id_, static_cast<uint32_t>(OpType::Recv));
};

Behavior handle_recv = [this](OpCompletion& op) {
    if (op.type != OpType::Recv) { return; }
    handle_server_message(op.fd, op.result);
    loop_->async_recv(server_fd_, recv_bufs, 1,
                      actor_id_, static_cast<uint32_t>(OpType::Recv));
};

Behavior handle_timer = [this](OpCompletion& op) {
    if (op.type != OpType::TimerFired) { return; }
    send_heartbeat();
};
```

**Heartbeat via `run_every()`**:
```cpp
uint64_t heartbeat_timer = loop_->run_every(actor_id_, 5000);  // every 5s
```

**Reconnection**: on connect failure, schedule backoff via `run_after()`.

---

### Task 31: Build and verify

```bash
ninja -C build 2>&1 | tail -30
```

Fix any remaining compilation errors.

---

### Task 32: Commit Phase 8

```bash
git add src/net/registrar_client.cpp
git commit -m "feat: migrate RegistrarClient off blocking threads"
```

---

## Final Verification

### Task 33: Full build on Linux

```bash
cmake -S . -B build -GNinja
ninja -C build
ctest --output-on-failure
```

### Task 34: Full build on macOS (if available)

Same commands on macOS machine. Expected: builds with GcdBackend instead of IoUringBackend.

### Task 35: Update project status memory

**Files:**
- Modify: `.claude/projects/-Users-skg7on-Workspace-Projects-HPActor/memory/project_status.md`

Add a note that the Phase 5 TCP/networking implementation has been extended with io_uring/libdispatch async I/O.

---

## Task Summary

| # | Phase | Files | Key Output |
|---|-------|-------|------------|
| 1 | AsyncIoBackend interface | async_io_backend.hpp, test | OpType/IoEvent enums, interface |
| 2 | CMake deps | CMakeLists.txt | liburing + libdispatch linked |
| 3 | IoUringBackend (Linux) | iouring_backend.hpp/cpp, test | Proactive async I/O on Linux |
| 4 | GcdBackend (macOS) | gcd_backend.hpp/cpp | Async I/O on macOS |
| 5 | EventLoop refactor | event_loop.hpp/cpp | EventLoop delegates to backend |
| 5.5 | ActorSystem enqueue | actor_system.hpp/cpp | enqueue_completion stub |
| 6 | TlsConnection + TcpTransport | tls_connection.cpp, tcp_transport.cpp | Truly async send/recv |
| 7 | RegistrarServer | registrar_server.cpp | No blocking threads |
| 8 | RegistrarClient | registrar_client.cpp | No blocking threads |

---

## Dependency Order

```
Phase 1 → Phase 2 → Phase 3 (Linux) ← parallel with Phase 4 (macOS)
                ↓
            Phase 5 → Phase 5.5 → Phase 6 → Phase 7 → Phase 8
```

Phases 3 and 4 can be developed in parallel (separate subagents). Phase 5 depends on both 3 and 4. Phase 5.5 depends on 5. Phases 6–8 depend on 5.5.
