# Zero-Copy Read Path: 2-copy pipeline with std::span

**Date:** 2026-04-28
**Owner:** SKG7ON
**Status:** Draft
**Type:** Feature Implementation

## Overview

Network data is copied 5 times between the socket and the actor mailbox. The goal is 2 copies:

```
socket → read_buffer_               ::read() directly — copy 1 (unavoidable)
  → span over frame                 zero-copy view into read_buffer_
    → WireFrame::payload (span)     zero-copy parse
      → TypedMessage::payload_      copy 2 — ownership boundary (unavoidable)
```

The key design shift: `read_callback` changes from `void(const bytes&)` to `void(int fd)`. The backend notifies "fd is readable" and the callback reads directly into its own accumulation buffer. No stack buffer, no intermediate `bytes` accumulation. All processing between uses `std::span<const uint8_t>` views.

---

## Current Copy Chain (5 copies)

```
socket
  → stack buf[65536]              ::read() — copy 1
    → accumulated bytes            service_read_handler insert — copy 2
      → read_buffer_ bytes         PlainConnection::handle_read insert — copy 3
        → frame bytes              extraction from read_buffer_ — copy 4
          → WireFrame::payload     WireFrame::decode — copy 5
```

## Target Copy Chain (2 copies)

```
socket
  → read_buffer_                   ::read() directly — copy 1
    → span over frame              zero-copy view (offset tracking into read_buffer_)
      → WireFrame::payload (span)  zero-copy parse
        → TypedMessage::payload_   copy 2 — mailbox ownership boundary
```

---

## Design

### Core Principle

**The processing layer must not own the memory; it must only look at it.** The backend notifies "fd is readable"; the connection reads directly into its own buffer. Frame extraction produces `std::span<const uint8_t>` views, not copies.

### Interface Changes

#### 1. `read_callback` (async_io_fwd.hpp)

Changes from data delivery to notification:

```cpp
// Before
using read_callback = std::function<void(const bytes&)>;

// After
using read_callback = std::function<void(int fd)>;
```

The backend just says "this fd has data." The callback owns the read.

#### 2. `frame_handler` (transport.hpp)

```cpp
// Before
using frame_handler = std::function<void(const bytes&)>;

// After
using frame_handler = std::function<void(std::span<const uint8_t>)>;
```

#### 3. `WireFrame` (frame.hpp)

Span-based parse:

```cpp
struct WireFrame {
    ActorAddress sender;
    ActorAddress receiver;
    bytes payload;               // owned copy (populated from span at ownership boundary)
    uint32_t flags = 0;
    uint64_t message_id = 0;
    uint32_t type_tag = 0;

    bytes encode() const;
    static WireFrame decode(std::span<const uint8_t> data);
};
```

`WireFrame::decode(span)` parses the header in-place from the span. The `payload` member is populated at the point where ownership is taken (when entering the actor mailbox).

### Implementation Details

#### service_read_handler (both backends)

Minimal — just looks up and invokes the callback:

```cpp
void Backend::service_read_handler(int fd) {
    read_callback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = read_handlers_.find(fd);
        if (it == read_handlers_.end()) return;
        cb = it->second;
    }
    cb(fd);
}
```

No stack buffer. No read loop. No data. Just the fd.

#### PlainConnection::on_fd_readable

Reads directly into `read_buffer_`, extracts frame spans with offset tracking:

```cpp
void PlainConnection::on_fd_readable(int fd) {
    // Copy 1: read directly into accumulation buffer
    size_t off = read_buffer_.size();
    read_buffer_.resize(off + 65536);
    while (true) {
        ssize_t n = ::read(fd, read_buffer_.data() + off,
                           read_buffer_.size() - off);
        if (n > 0) {
            off += n;
            read_buffer_.resize(off + 65536);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
    }
    read_buffer_.resize(off);

    // Zero-copy: extract complete frames as span views into read_buffer_
    size_t offset = 0;
    while (read_buffer_.size() - offset >= 4) {
        size_t frame_len = (read_buffer_[offset] << 24) |
                           (read_buffer_[offset+1] << 16) |
                           (read_buffer_[offset+2] << 8) |
                           read_buffer_[offset+3];
        if (read_buffer_.size() - offset < 4 + frame_len) break;
        std::span<const uint8_t> frame(&read_buffer_[offset + 4], frame_len);
        offset += 4 + frame_len;
        if (frame_handler_) frame_handler_(frame);
    }
    if (offset > 0)
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + offset);
}
```

No intermediate copies between `::read()` and `frame_handler_`. The frame span points directly into `read_buffer_`.

#### ConnectionPool::on_frame_received

```cpp
void ConnectionPool::on_frame_received(std::span<const uint8_t> frame_data) {
    WireFrame frame = WireFrame::decode(frame_data);
    // ... existing routing logic, unchanged ...
}
```

#### ActorSystem::deliver_remote — the ownership boundary

```cpp
void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    // Copy 2: span → owning TypedMessage for the mailbox
    TypedMessage msg(static_cast<TypeTag>(frame.type_tag),
                     bytes(frame.payload.begin(), frame.payload.end()));
    msg.set_sender_address(frame.sender);
    deliver_local(frame.receiver.id, std::move(msg));
}
```

This is the only additional copy — necessary because the mailbox message must outlive `read_buffer_`.

---

## Copy Budget

| Step | Copy? | Why |
|------|-------|-----|
| kernel → `read_buffer_` | Copy 1 | `::read()` requires a destination buffer |
| `read_buffer_` → span (frame extraction) | Zero | Offset tracking; span points into backing vector |
| span → `WireFrame` parse | Zero | `WireFrame::decode(span)` parses header fields in-place |
| span → `TypedMessage::payload_` | Copy 2 | Mailbox message must outlive network buffer |

---

## What Is Eliminated

| Copy | How |
|------|-----|
| stack buffer → accumulated `bytes` | `service_read_handler` no longer reads or accumulates |
| accumulated → `read_buffer_` | `PlainConnection::on_fd_readable` reads directly into `read_buffer_` |
| frame extraction from `read_buffer_` | Spans slice into `read_buffer_` with offset tracking |
| WireFrame payload from frame data | `WireFrame::decode(span)` parses in-place |

---

## File Changes

| File | Change |
|------|--------|
| `include/hpactor/net/async_io_fwd.hpp` | `read_callback` → `void(int fd)` |
| `include/hpactor/net/transport.hpp` | `frame_handler` → `std::span<const uint8_t>` |
| `include/hpactor/net/plain_connection.hpp` | `handle_read` → `on_fd_readable(int fd)` |
| `src/net/plain_connection.cpp` | Read directly into `read_buffer_`, span extraction |
| `src/net/reactor/epoll_backend.cpp` | `service_read_handler` → just invoke `cb(fd)` |
| `src/net/reactor/kqueue_backend.cpp` | `service_read_handler` → just invoke `cb(fd)` |
| `include/hpactor/net/connection_pool.hpp` | `on_frame_received` → span |
| `src/net/connection_pool.cpp` | `on_frame_received` → span, `WireFrame::decode(span)` |
| `include/hpactor/net/frame.hpp` | `WireFrame::decode(span)` overload |
| `src/net/frame.cpp` | `WireFrame::decode(span)` implementation |
| `src/net/tcp_transport.cpp` | Handler lambdas → span |
| `src/actor/actor_system.cpp` | `deliver_remote` copy at ownership boundary |
| Test files | Callback signature updates |

---

## Testing

1. **Unit**: Socket pair — write framed data, verify `frame_handler_` receives correct span contents.
2. **Regression**: All 61 existing tests pass unchanged.
3. **No new dependencies**: `std::span` is C++20, already in use.
