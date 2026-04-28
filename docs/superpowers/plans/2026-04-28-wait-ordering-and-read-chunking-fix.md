# Fix: wait() ordering and service_read_handler chunking

**Date:** 2026-04-28
**Owner:** SKG7ON
**Status:** Draft
**Based on:** `docs/superpowers/plans/2026-04-28-unified-read-abstraction-impl.md`

---

## Bug 1: EpollBackend::wait() — process_pending_op drains data before service_read_handler

### Root Cause

`epoll_backend.cpp:811-818`:

```cpp
} else if (fd >= 0) {
    process_pending_op(fd, events[i].events);   // drains if pending recv exists
    if (events[i].events & EPOLLIN) {
        service_read_handler(fd);               // unconditionally called — stale
    }
}
```

`process_pending_op` calls `::readv()` when the fd has a pending `OpType::Recv`. After that drain, `service_read_handler` runs for the same fd and finds no data. In practice a fd shouldn't have both a pending recv and a read handler, but the code path allows the double-read.

### Fix

Gate `service_read_handler` on the absence of a pending op:

```cpp
} else if (fd >= 0) {
    if (pending_ops_.find(fd) != pending_ops_.end()) {
        process_pending_op(fd, events[i].events);
    } else if (events[i].events & EPOLLIN) {
        service_read_handler(fd);
    }
}
```

**File:** `src/net/reactor/epoll_backend.cpp` — lines 811-818

### KqueueBackend Status

Not affected. The `goto next_event` after processing a pending recv correctly jumps over the `service_read_handler` fallback.

---

## Bug 2: service_read_handler calls callback per ::read() chunk

### Current Behavior

`service_read_handler` loops `::read()` → callback per chunk:

```
::read() → 2 bytes  → cb({2 bytes})    // partial header
::read() → 100 bytes → cb({100 bytes})  // header + partial payload
::read() → EAGAIN → break
```

`PlainConnection::handle_read` handles this correctly (accumulates in `read_buffer_`), but each callback invocation copies data into `read_buffer_` separately. The repeated callback calls are unnecessary overhead — all available data should be delivered in one callback.

### Fix

Accumulate all data from the `::read()` loop into a single `bytes` buffer, then call the callback once:

```cpp
void Backend::service_read_handler(int fd) {
    read_callback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = read_handlers_.find(fd);
        if (it == read_handlers_.end()) return;
        cb = it->second;
    }

    bytes accumulated;
    uint8_t buf[65536];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            accumulated.insert(accumulated.end(), buf, buf + n);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
    }

    if (!accumulated.empty()) {
        cb(accumulated);
    }
}
```

**Files:**
- `src/net/reactor/epoll_backend.cpp` — `service_read_handler()`
- `src/net/reactor/kqueue_backend.cpp` — `service_read_handler()`

---

## File Summary

| File | Change |
|------|--------|
| `src/net/reactor/epoll_backend.cpp` | Bug 1: if/else gating. Bug 2: accumulate into single `bytes` before callback |
| `src/net/reactor/kqueue_backend.cpp` | Bug 2 only: accumulate into single `bytes` before callback |

---

## Verification

- [ ] Build: `ninja -C build` — clean
- [ ] Tests: `ctest --output-on-failure` — 61/61 passing
- [ ] No behavioral change — `handle_read` receives same data, just in fewer callback invocations
