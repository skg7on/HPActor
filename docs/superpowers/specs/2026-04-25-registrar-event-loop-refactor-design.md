# Registrar Event-Loop Refactor Design

**Date:** 2026-04-25
**Status:** Design
**Author:** HPActor Team

## Context

The current `RegistrarServer`, `RegistrarClient`, and `UdpRegistrar` implementations use blocking synchronous socket I/O via dedicated threads (`accept_thread_`, `connection_thread_`, `heartbeat_thread_`). This conflicts with the project's async-first architecture where `EventLoop` and `AsyncIoBackend` provide unified async I/O across Linux (io_uring) and macOS (GCD/kqueue).

The goal is to refactor these components to use the existing async I/O framework, eliminating native socket operations in favor of `EventLoop::add_fd()`, `AsyncIoBackend::async_*()` operations, and callbacks.

## Existing Async I/O Framework

The project already has a working async I/O infrastructure:

- **EventLoop** (`event_loop.hpp`): Unified interface wrapping platform-specific backends. Provides `add_fd()`, `run_after()`, `run_every()`, `cancel_timer()`, and completion routing.
- **AsyncIoBackend** (`async_io_backend.hpp`): Platform backends (IoUringBackend on Linux, GcdBackend on macOS). Provides `async_send()`, `async_recv()`, `async_accept()`, `async_connect()`, `async_recvfrom()`, `async_sendto()`.
- **Acceptor** (`acceptor.hpp`): TCP listener with callback-based accept, already integrated with EventLoop.
- **PlainConnection** (`plain_connection.hpp`): Async TCP connection with 4-byte length framing.

## Design: RegistrarConnection Class

Create a new `RegistrarConnection` class that wraps a connected TCP socket and knows the registrar's custom framing protocol.

### Registrar Framing Protocol

Each message: `[Magic: 4][Version: 1][Type: 1][Length: 4][Payload: N]`

- Magic: `0x5247xxxx` (RG + version, where version=1 → 0x52475201? No, Magic=0x52474354? Let me check existing...) Actually existing `TcpRegistrarMagic` is used. We preserve the existing protocol format.
- Version: `TcpRegistrarVersion` (currently 1)
- Type: `TcpMessageType` enum (Register=0, Accept=1, Heartbeat=2, NodeJoin=3, NodeLeave=4, Error=5)
- Length: payload length in bytes (big-endian uint32)

### Class Definition

```cpp
// In include/hpactor/net/registrar.hpp (new class, alongside existing structs)

// RegistrarConnection - async TCP connection for registrar protocol
class RegistrarConnection : public std::enable_shared_from_this<RegistrarConnection> {
public:
    using message_handler = std::function<void(TcpMessageType, const bytes&)>;
    using disconnect_handler = std::function<void()>;
    using send_complete_handler = std::function<void(int result)>;

    // Create from accepted server socket
    static std::shared_ptr<RegistrarConnection> accepted(int fd,
                                                         CommunicationEndpoint remote_endpoint,
                                                         EventLoop* loop);

    // Create as client connection
    static std::shared_ptr<RegistrarConnection> connecting(int fd,
                                                            CommunicationEndpoint remote_endpoint,
                                                            EventLoop* loop);

    ~RegistrarConnection();

    // Set handlers
    void set_message_handler(message_handler h);
    void set_disconnect_handler(disconnect_handler h);
    void set_send_complete_handler(send_complete_handler h);

    // Send registrar message
    void send_message(TcpMessageType type, const bytes& payload);

    // Close connection
    void close();

    // Get remote endpoint
    CommunicationEndpoint remote_endpoint() const { return remote_endpoint_; }

    // Get fd
    int fd() const { return fd_; }

private:
    RegistrarConnection(CommunicationEndpoint remote_endpoint, EventLoop* loop, int fd);

    void register_with_loop();
    void handle_read_event();
    void handle_write_event();
    void handle_payload_read(TcpMessageType type, uint32_t payload_len);
    void flush_write_buffer();

    enum class ReadState {
        ReadingHeader,
        ReadingPayload
    };

    CommunicationEndpoint remote_endpoint_;
    EventLoop* loop_ = nullptr;
    int fd_ = -1;

    ReadState read_state_ = ReadState::ReadingHeader;
    size_t header_bytes_read_ = 0;
    bytes header_buffer_;  // 10 bytes for header

    TcpMessageType current_type_;
    size_t payload_bytes_read_ = 0;
    bytes payload_buffer_;

    bytes read_buffer_;
    bytes write_buffer_;
    bool is_sending_ = false;

    message_handler message_handler_;
    disconnect_handler disconnect_handler_;
    send_complete_handler send_complete_handler_;
};
```

### State Machine

```
              ┌─────────────────────────────────────────────────────────────┐
              │                                                             │
              ▼                                                             │
        ReadingHeader ◄───────────────────────────────────────────────────┐
              │                                                             │
              │ header complete (10 bytes)                                  │
              ▼                                                             │
        Parse: magic, version, type, length                                 │
              │                                                             │
              │ valid? ──── No ────► close connection                       │
              │                                                             │
              │ Yes                                                         │
              ▼                                                             │
        ReadingPayload ◄───────────────────────────────────────────────────┘
              │
              │ payload complete
              ▼
        Deliver to message_handler_
```

### Async Send Pattern

Follows `PlainConnection::flush_write_buffer()` pattern:

```cpp
void RegistrarConnection::flush_write_buffer() {
    if (fd_ < 0 || loop_ == nullptr || write_buffer_.empty()) {
        return;
    }

    is_sending_ = true;

    struct iovec iov;
    iov.iov_base = write_buffer_.data();
    iov.iov_len = write_buffer_.size();

    // encode user_data: fd (lower 32) | actor (next 16) | op_type (upper 8)
    loop_->backend()->async_send(fd_, &iov, 1, ActorId(0),
                                 static_cast<uint32_t>(OpType::Send));
}
```

Send completion is routed via `EventLoop::completion_callback_`, which calls `RegistrarConnection::handle_send_completion()`.

### Send Completion Handler

```cpp
void RegistrarConnection::handle_send_completion(int result) {
    if (send_complete_handler_) {
        send_complete_handler_(result);
    }
    is_sending_ = false;

    if (result < 0) {
        close();
        return;
    }

    // Remove sent bytes from write buffer
    if (static_cast<size_t>(result) >= write_buffer_.size()) {
        write_buffer_.clear();
    } else {
        write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + result);
    }

    // Continue flushing if more data
    if (!write_buffer_.empty()) {
        flush_write_buffer();
    }
}
```

## Design: Refactored RegistrarServer

### Changes

1. **Remove** `accept_thread_`, `tcp_socket_`, and `clients_` mutex map
2. **Add** `EventLoop* loop_` member and `std::unordered_map<int, std::shared_ptr<RegistrarConnection>> clients_`
3. **Use** `Acceptor` for TCP listening (already async)
4. **Replace** blocking `handle_accept()` with async pattern using `RegistrarConnection::accepted()`

### Architecture

```
RegistrarServer
├── Acceptor (listening socket, registered with EventLoop)
├── EventLoop* loop_
├── std::unordered_map<int, RegistrarConnectionPtr> clients_
│   └── Each connection handles:
│       ├── async recv for incoming messages
│       ├── message_handler_ → handle_tcp_message()
│       ├── send_message() for responses/broadcasts
│       └── disconnect_handler_ → remove from clients_
└── NodeRegistry registry_
```

### Connection Lifecycle

1. `Acceptor::accept_handler_` fires on new connection
2. Create `RegistrarConnection::accepted(fd, remote_endpoint, loop_)`
3. Register `message_handler_`: calls `handle_tcp_message(type, payload)`
4. Register `disconnect_handler_`: removes from clients_, calls `broadcast_node_left()`
5. Connection reads messages asynchronously via `handle_read_event()`
6. On disconnect, connection auto-removed from clients_ map via destructor/close

### Updated Methods

```cpp
void RegistrarServer::start() {
    if (running_.load()) return;
    running_.store(true);

    // Use Acceptor for TCP listening (already async)
    acceptor_.set_accept_handler([this](int fd, CommunicationEndpoint remote_ep) {
        handle_accept(fd, remote_ep);
    });
    acceptor_.listen(config_.tcp_port);

    // Create UDP socket for resolution (keep for now, refactor later if needed)
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    // ... bind UDP ...
}

void RegistrarServer::handle_accept(int client_fd, CommunicationEndpoint remote_endpoint) {
    auto conn = RegistrarConnection::accepted(client_fd, remote_endpoint, loop_);

    conn->set_message_handler([this, conn](TcpMessageType type, const bytes& payload) {
        handle_tcp_message(conn->fd(), type, payload);
    });

    conn->set_disconnect_handler([this, conn]() {
        handle_disconnect(conn);
    });

    clients_[client_fd] = conn;
}
```

### Broadcast Implementation

```cpp
void RegistrarServer::broadcast_node_joined(CommunicationEndpoint endpoint, const NodeEndpoint& ep) {
    bytes payload = encode_node_join_payload(endpoint, ep);

    // Send to all clients except the sender
    for (auto& [fd, conn] : clients_) {
        (void)endpoint; // FIXME: need to track which endpoint each connection represents
        if (fd >= 0) {
            conn->send_message(TcpMessageType::NodeJoin, payload);
        }
    }
}
```

**Note:** The current `clients_` maps `fd → int`. Need to track which endpoint each fd represents for the "except sender" logic. Options:
1. Change to `std::unordered_map<CommunicationEndpoint, RegistrarConnectionPtr>` (endpoint is the key)
2. Add a reverse map or store endpoint in connection

**Recommendation:** Change `clients_` to `std::unordered_map<CommunicationEndpoint, RegistrarConnectionPtr>` since endpoint is the primary identity. The fd is accessed via `conn->fd()`.

### Node Tracking

The server needs to know the endpoint for each connected client to:
1. Look up client by fd for `handle_tcp_message`
2. Look up client by endpoint for broadcast exclusion
3. Remove from registry on disconnect

Change `clients_` from `std::unordered_map<int, int>` to `std::unordered_map<CommunicationEndpoint, RegistrarConnectionPtr>`.

## Design: Refactored RegistrarClient

### Changes

1. **Remove** `connection_thread_`, `heartbeat_thread_`, `tcp_socket_`
2. **Add** `EventLoop* loop_`, `RegistrarConnectionPtr server_connection_`
3. **Replace** blocking `connect_to_server()` with async connect using `AsyncIoBackend::async_connect()`
4. **Use** `EventLoop::run_every()` for heartbeat timer instead of dedicated thread

### Architecture

```
RegistrarClient
├── EventLoop* loop_
├── RegistrarConnectionPtr server_connection_
├── NodeRegistry* shared_registry_
└── std::unique_ptr<UdpSocket> udp_socket_ (for DNS/resolution, separate from TCP registrar)
```

### Async Connect Flow

```cpp
void RegistrarClient::connect_async(CommunicationEndpoint server_endpoint) {
    // Create socket
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Resolve and prepare address
    struct sockaddr_in addr = resolve_and_prepare_address(server_endpoint);

    // Initiate async connect
    loop_->backend()->async_connect(fd, &addr, sizeof(addr), ActorId(0));

    // Note: connect completion delivered via EventLoop completion callback
    // We need to register a handler for OpType::Connect completion

    // For now: use blocking connect with short timeout as interim step
    // Full async connect requires: completion routing by fd + connect callback registration
}
```

**Note:** The `AsyncIoBackend::async_connect()` exists but completion routing requires `OpCompletion` handling similar to send/recv. The `TcpTransport` uses blocking `connect()` with `O_NONBLOCK` as an interim pattern.

### Heartbeat Timer

```cpp
void RegistrarClient::start() {
    if (running_.load()) return;
    running_.store(true);

    // Schedule heartbeat timer via EventLoop
    heartbeat_timer_ = loop_->run_every([this]() {
        if (server_connection_ && connected_.load()) {
            server_connection_->send_message(TcpMessageType::Heartbeat, {});
        }
    }, static_cast<int>(config_.heartbeat_interval.count()));
}
```

### Reconnection Logic

Keep existing exponential backoff logic but use `EventLoop::run_after()` for retry delays:

```cpp
void RegistrarClient::schedule_reconnect(int delay_ms) {
    loop_->run_after([this]() {
        attempt_connection();
    }, delay_ms);
}
```

## Design: Async UDP for UdpRegistrar

### Changes

1. **Add** UDP socket registered with EventLoop
2. **Replace** blocking `recvfrom` with async read via `add_fd()` + `has_event()`
3. **Use** `EventLoop::run_every()` for expiration cleanup

### UDP Read Pattern

The current `UdpRegistrar` handles UDP packets via `handle_udp_packet()` called from... nowhere currently (the socket isn't integrated with EventLoop).

**New Pattern:**

```cpp
class UdpRegistrar {
    // In start_server_mode():
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    // bind...
    loop_->add_fd(udp_socket_, EventLoop::Event::Read);

    // Set a read callback on the EventLoop or handle via has_event() in run loop
    // Actually, we need to integrate UDP with the completion callback system

    // Option 1: Register udp_socket_ with loop, poll has_event() in process_completions
    // Option 2: Add async_recvfrom to AsyncIoBackend and route completions
};
```

**Decision:** The `AsyncIoBackend` already has `async_recvfrom`. We should use it.

```cpp
// In UdpRegistrar::start_server_mode():
udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
// bind...
loop_->add_fd(udp_socket_, EventLoop::Event::Read);

// For UDP, we need a different pattern since recvfrom needs address info
// Option: Use a separate thread for UDP receive that delivers to a callback
// OR: Route UDP completions separately

// Recommendation: Keep UDP with select() for now, focus on TCP refactor
// The UDP path is simpler and less performance-critical
```

**Revised Decision:** Focus on TCP refactor first. UDP async is more complex (requires `async_recvfrom` with address storage, different completion routing). Keep UDP with `select()` for now in initial refactor. Future work can address UDP async.

## Key Interface Changes

### RegistrarServer

| Before | After |
|--------|-------|
| `std::thread accept_thread_` | Removed (Acceptor handles async accept) |
| `std::mutex clients_mutex_` | `std::mutex` still needed for `clients_` map |
| `std::unordered_map<CommunicationEndpoint, int> clients_` | `std::unordered_map<CommunicationEndpoint, RegistrarConnectionPtr> clients_` |
| Blocking `recv()` in message loop | Async via `RegistrarConnection` callbacks |
| `broadcast_node_joined()` iterates fds, does `send()` | `conn->send_message()` |

### RegistrarClient

| Before | After |
|--------|-------|
| `std::thread connection_thread_` | Event-driven via callbacks |
| `std::thread heartbeat_thread_` | `EventLoop::run_every()` timer |
| Blocking `connect()`, `send()`, `recv()` | Async via `RegistrarConnection` |
| `send_registration()` builds raw bytes + `send()` | `conn->send_message(TcpMessageType::Register, payload)` |

### UdpRegistrar

| Before | After |
|--------|-------|
| Native UDP sockets with `select()` | Initial: keep as-is, UDP async deferred |

## Error Handling

1. **Connection close:** `RegistrarConnection` calls `disconnect_handler_`. Server removes from clients, broadcasts NodeLeave.
2. **Send failure:** `handle_send_completion()` calls `close()`, triggers disconnect flow.
3. **Read error:** `handle_read_event()` detects `recv()` <= 0, calls `close()`.
4. **Heartbeat timeout:** Client detects via missing responses (server should disconnect unresponsive clients after threshold).

## Testing Considerations

1. **Unit tests:** Test `RegistrarConnection` framing in isolation
2. **Integration tests:** Two-process test with async registrar
3. **Race conditions:** Verify client removal from map under concurrent disconnect + broadcast

## File Changes

| File | Change |
|------|--------|
| `include/hpactor/net/registrar.hpp` | Add `RegistrarConnection` class declaration |
| `src/net/registrar.cpp` | Add `RegistrarConnection` implementation |
| `src/net/registrar_server.cpp` | Refactor to use `RegistrarConnection` + `Acceptor` |
| `src/net/registrar_client.cpp` | Refactor to use `RegistrarConnection` + EventLoop timers |

## Implementation Order

1. **Phase 1:** Implement `RegistrarConnection` class
   - Framing state machine
   - Async send/recv via `EventLoop`
   - Message delivery callback
   - Disconnect callback

2. **Phase 2:** Refactor `RegistrarServer`
   - Replace `accept_thread_` with `Acceptor`
   - Use `RegistrarConnection` for client connections
   - Update broadcast to use `send_message()`
   - Remove blocking socket code

3. **Phase 3:** Refactor `RegistrarClient`
   - Replace `connection_thread_` with async connect pattern
   - Replace `heartbeat_thread_` with `EventLoop::run_every()`
   - Use `RegistrarConnection` for server communication

4. **Phase 4 (Future):** Async UDP
   - Integrate `async_recvfrom`/`async_sendto` with EventLoop
   - Remove `select()` from UDP operations

## Open Questions

1. **Connect completion routing:** `AsyncIoBackend::async_connect()` exists but requires completion routing. Currently TcpTransport uses blocking connect. Need to decide if RegistrarClient should use blocking connect (simpler) or full async connect (consistent).

   **Decision:** Use blocking connect with `O_NONBLOCK` as interim, same pattern as TcpTransport. Full async connect completion routing is a separate enhancement.

2. **UDP async:** Deferred to future work. The UDP path is simpler and blocking is less problematic for single-socket request/response.

3. **Endpoint tracking in clients_ map:** Change key from `fd` to `CommunicationEndpoint` for cleaner lookup and broadcast exclusion logic.
