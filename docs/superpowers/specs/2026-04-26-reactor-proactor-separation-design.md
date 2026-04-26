# Reactor/Proactor Separation Design

**Date:** 2026-04-26
**Status:** Approved
**Author:** HPActor

## Overview

Separate the async I/O backends into two distinct patterns (Reactor and Proactor) at the architecture level, while maintaining a consistent Actor messaging API (`context()->send()` + `become(Behavior)`) regardless of which mode is used.

## Background

The current `AsyncIoBackend` interface conflates two fundamentally different I/O paradigms:

- **Reactor** (epoll, kqueue): Poll for fd readiness, then synchronous I/O
- **Proactor** (io_uring, GCD): Submit I/O requests, kernel performs asynchronously, completion callback

Additionally, `set_read_handler()` is a Reactor-specific concept that has no place in the Proactor interface. This creates confusion and forces Proactor backends to implement a pattern they don't naturally use.

## Goals

1. Clean separation of Reactor and Proactor at the interface level
2. Actor messaging API remains consistent (send + Behavior handlers)
3. Compile-time policy selection via template
4. No runtime overhead for mode selection

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    ActorSystem                          │
│         (template<ProactorPolicy | ReactorPolicy>)      │
└───────────────────────┬─────────────────────────────────┘
                        │
         ┌──────────────┴──────────────┐
         ▼                             ▼
┌─────────────────┐           ┌─────────────────┐
│ ProactorPolicy  │           │  ReactorPolicy  │
└────────┬────────┘           └────────┬────────┘
         │                             │
         ▼                             ▼
┌─────────────────┐           ┌─────────────────┐
│IProactorBackend│           │ IReactorBackend │
└────────┬────────┘           └────────┬────────┘
         │                             │
         ▼                             ▼
┌─────────────────┐           ┌─────────────────┐
│  IoUringBackend │           │  EpollBackend   │
│  GcdBackend    │           │  KqueueBackend  │
└─────────────────┘           └─────────────────┘
         │                             │
         ▼                             ▼
┌─────────────────┐           ┌─────────────────┐
│ProactorDispatch │           │ReactorDispatch  │
│ (completion →   │           │(readiness →     │
│  actor mailbox) │           │ Behavior handler│
└─────────────────┘           └─────────────────┘
```

## Interface Separation

### Shared Types (async_io_fwd.hpp)

```cpp
enum class OpType : uint32_t {
    Send = 1, Recv = 2, Accept = 3, Connect = 4,
    TimerFired = 5, RecvFrom = 6, SendTo = 7
};

struct OpCompletion {
    ActorId actor;
    OpType type;
    int fd;
    int result;        // >= 0 bytes on success, < 0 errno on failure
    uint64_t user_data;
    sockaddr_in src_addr = {};
    socklen_t src_addr_len = 0;
};
```

### IProactorBackend (proactor_backend.hpp)

Pure completion-based interface for io_uring and GCD:

```cpp
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

    virtual int wait(int timeout_ms) = 0;          // blocks until completions
    virtual void process_completions() = 0;         // drain completion queue
    virtual void deliver_completion(OpCompletion) = 0;
};
```

### IReactorBackend (reactor_backend.hpp)

Pure readiness-based interface for epoll and kqueue:

```cpp
class IReactorBackend {
public:
    virtual ~IReactorBackend() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual bool add_fd(int fd, IoEvent events) = 0;
    virtual bool update_fd(int fd, IoEvent events) = 0;
    virtual bool remove_fd(int fd) = 0;

    virtual int wait(int timeout_ms) = 0;          // blocks on poll
    virtual void process_events() = 0;              // process readiness events

    virtual int register_buffer(const void* addr, size_t len) = 0;
    virtual bool unregister_buffer(int buffer_id) = 0;

    // Timers managed externally via EventLoop
};

// IoEvent flags
enum class IoEvent : uint32_t {
    Read = 1 << 0,
    Write = 1 << 1,
};
```

## Dispatcher Design

### ProactorDispatcher

Receives completions from the Proactor backend and routes to actor mailboxes:

```cpp
class ProactorDispatcher {
    void on_completion(OpCompletion completion) {
        switch (completion.type) {
            case OpType::TimerFired:
                event_loop_.deliver_timer(completion.user_data);
                break;
            case OpType::Send:
            case OpType::Recv:
            case OpType::Accept:
            case OpType::Connect:
                actor_mailbox(completion.actor).enqueue(completion);
                break;
        }
    }

    EventLoop& event_loop_;
};
```

### ReactorDispatcher

Reads readiness events from the EventLoop and invokes Behavior handlers:

```cpp
class ReactorDispatcher {
    void on_readiness(int fd, IoEvent events) {
        ActorId actor = fd_to_actor_[fd];

        if (events & IoEvent::Read) {
            bytes data = sync_read(fd);  // blocking read
            actor_mailbox(actor).enqueue(Message{data});
        }
        if (events & IoEvent::Write) {
            // Handle write readiness
        }
    }

    void on_timer(uint64_t handle) {
        event_loop_.deliver_timer(handle);
    }

    std::unordered_map<int, ActorId> fd_to_actor_;
    EventLoop& event_loop_;
};
```

## ActorSystem Template

```cpp
// Policy tags
struct ProactorPolicy {
    using Backend = /* platform-default Proactor */;
};
struct ReactorPolicy {
    using Backend = /* platform-default Reactor */;
};

// ActorSystem is parameterized by policy
template<typename Policy = ProactorPolicy>
class ActorSystemImpl {
    // Common fields (spawn, registry, clock, etc.)
    typename Policy::Backend backend_;
    Dispatcher dispatcher_;

public:
    // All existing ActorSystem methods unchanged
    Actor spawn(...) { ... }
    void send(ActorId target, Message msg) {
        backend_.async_send(get_fd(target), ...);
    }
};
```

### Backend Selection

Platform-specific defaults via CMake:

```cmake
# CMakeLists.txt
if(ENABLE_PROACTOR)
    if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
        set(DEFAULT_BACKEND "iouring")
    else()
        set(DEFAULT_BACKEND "gcd")
    endif()
else()
    if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
        set(DEFAULT_BACKEND "epoll")
    else()
        set(DEFAULT_BACKEND "kqueue")
    endif()
endif()
```

User compiles with `-DENABLE_PROACTOR=ON/OFF` to select mode.

## Message Path

### Proactor Path

```
Actor A: context()->send(addr, msg)
        │
        ▼
IoUringBackend::async_send() → SQE submitted to kernel
        │
        ▼ (later: completion)
IoUringBackend::process_completions() → OpCompletion
        │
        ▼
ProactorDispatcher::on_completion() → ActorMailbox::enqueue(msg)
        │
        ▼
Actor B's Behavior invoked (as today)
```

### Reactor Path

```
Actor A: context()->send(addr, msg)
        │
        ▼
EpollBackend stores pending write (actor id in user_data)
        │
        ▼ (later: fd ready)
EventLoop::wait() → EpollBackend::process_events()
        │
        ▼
ReactorDispatcher::on_readiness(fd, EPOLLOUT)
        │
        ▼
sync write() → bytes sent
        │
        ▼
ActorMailbox::enqueue(ack) → Actor A's Behavior
```

Both paths deliver to `ActorMailbox` → Actor programming model unchanged.

## File Structure

### New Organization

```
include/hpactor/
├── actor_system.hpp              # ActorSystem template
├── actor_system_proactor.hpp    # Proactor specialization
├── actor_system_reactor.hpp     # Reactor specialization
├── net/
│   ├── async_io_fwd.hpp         # OpType, OpCompletion (shared)
│   ├── proactor_backend.hpp     # IProactorBackend interface
│   ├── reactor_backend.hpp      # IReactorBackend interface
│   ├── proactor_dispatcher.hpp  # ProactorDispatcher
│   ├── reactor_dispatcher.hpp   # ReactorDispatcher
│   └── event_loop.hpp           # EventLoop (Reactor only)
```

### Backend Implementation

```
src/net/
├── proactor/
│   ├── iouring_backend.cpp      # IoUringBackend : IProactorBackend
│   └── gcd_backend.cpp           # GcdBackend : IProactorBackend
├── reactor/
│   ├── epoll_backend.cpp         # EpollBackend : IReactorBackend
│   └── kqueue_backend.cpp        # KqueueBackend : IReactorBackend
├── event_loop.cpp                # Reactor event loop
└── dispatcher.cpp                # Dispatcher implementations
```

### Files to Delete

- `include/hpactor/net/async_io_backend.hpp` — replaced by two interfaces

## What Stays Unchanged

- `Actor`, `ActorAddress`, `ActorId` types
- `ActorContext::send()` API
- `Behavior` and `message_handler`
- `ActorMailbox` and message delivery
- Connection, Acceptor, TLS, ConnectionPool (they use backend interface)
- All 51 tests

## What Changes

1. `AsyncIoBackend` split into `IProactorBackend` + `IReactorBackend`
2. `set_read_handler()` removed entirely (Reactor uses fd_actors_ map instead)
3. `async_send_fixed/recv_fixed` moved to Proactor only (Reactor doesn't support)
4. `ActorSystem` becomes a template parameterized by policy
5. Backend files reorganized into `proactor/` and `reactor/` subdirs

## Implementation Order

1. Create `async_io_fwd.hpp` with shared types
2. Create `proactor_backend.hpp` and `reactor_backend.hpp` interfaces
3. Move backend implementations to new directories
4. Implement `ProactorDispatcher` and `ReactorDispatcher`
5. Make `ActorSystem` a template with specializations
6. Update CMake build system for policy selection
7. Update all consumers of `AsyncIoBackend` to use correct interface
8. Run tests to verify no regressions

## Rationale

**Why compile-time over runtime?**
- Zero overhead for mode selection (template specialization at compile time)
- No virtual dispatch overhead for backend calls
- Compiler can inline appropriately
- CMake flag is simple and well-understood

**Why remove `set_read_handler`?**
- It's a Reactor concept (callback registration) that has no meaning in Proactor
- Proactor backends were implementing it as a stub or work-around
- Clean separation requires each interface to have only operations that make sense for that pattern
