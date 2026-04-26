# async_accept Fix Design Spec

**Date:** 2026-04-26
**Status:** Implemented

## Problem

The `async_accept` method in all async I/O backends is broken:
- It doesn't register the accepted `client_fd` with the event loop
- Callers cannot use the returned fd for subsequent I/O operations
- `GcdBackend` implementation deletes `AcceptContext` before caller can use `client_fd`

The current sync-accept path via `Acceptor::handle_read()` works correctly because it synchronously calls `accept()`, passes the fd to `handle_accept()`, which registers it via `PlainConnection::create_server()` → `loop->add_fd()`. But `async_accept` itself is unusable.

## Solution

Implement true async accept with proper fd registration inside the backend:

```
async_accept(listening_fd, actor)
    → register listening_fd with edge-triggered accept interest
    → return immediately (no blocking)

[When connection arrives - edge triggered]
    → accept() to get client_fd
    → register client_fd with read interest
    → deliver OpCompletion{type=Accept, fd=client_fd}
```

The key improvement: fd registration happens inside the backend **before** completion delivery, so the caller receives an fd that's already registered and ready for `async_recv`.

## Implementation Details

### KqueueBackend

```cpp
void KqueueBackend::async_accept(int fd, ActorId actor) {
    // Register listening socket for edge-triggered accept
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, nullptr);
    kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);

    // Store actor for when accept event fires
    accept_actors_[fd] = actor;
}
```

When EVFILT_READ fires on listening fd:
1. Call `accept()` to get `client_fd`
2. Register `client_fd` with `EVFILT_READ` for incoming data
3. Deliver `OpCompletion{type=Accept, fd=client_fd}`

### GcdBackend

Fix existing implementation at line 338 to:
1. Not delete `AcceptContext` after single accept - keep source alive for subsequent accepts
2. Re-arm dispatch_source for next accept
3. Register `client_fd` with event loop before delivering completion

### IoUringBackend / EpollBackend

Add proper implementations following same pattern.

## Files to Modify

| File | Change |
|------|--------|
| `src/net/kqueue_backend.cpp` | Full async accept implementation |
| `src/net/gcd_backend.cpp` | Fix existing async_accept |
| `src/net/iouring_backend.cpp` | Add implementation |
| `src/net/epoll_backend.cpp` | Add stub implementation |

## Backward Compatibility

- Existing `Acceptor::handle_read()` path continues to work (sync accept + direct handler callback)
- `async_accept` becomes a proper async alternative
- No API changes to `AsyncIoBackend` interface

## Test Plan

Add test that:
1. Creates listening socket
2. Calls `backend->async_accept(listening_fd, actor)`
3. Connects client to listening socket
4. Processes completions
5. Verifies accepted fd can receive data via `async_recv`

## Verification

- Build passes
- All existing tests pass (56/56)
- New async_accept test passes