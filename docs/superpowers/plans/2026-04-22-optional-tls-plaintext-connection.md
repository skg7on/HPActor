# Optional TLS / Plain Text Connection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make TLS optional in TcpTransport/ConnectionPool. Default to plain text communication without TLS. Allow TLS to be enabled via configuration when secure communication is needed.

**Architecture:** Introduce `PlainConnection` for raw socket communication without TLS. Both `PlainConnection` and `TlsConnection` will inherit from `Connection` base class. Modify `ConnectionPool` to manage both connection types via `ConnectionPtr`. Add `use_tls` flag to `PoolConfig`. Fix fd assignment for outbound connections in `TcpTransport::connect()`. Update `TcpTransport::connections_` map to use `ConnectionPtr` base type for completion routing.

**Tech Stack:** C++20, EventLoop, KqueueBackend/EpollBackend, OpenSSL (when TLS enabled)

---

## File Structure

**Files to create:**
- `include/hpactor/net/plain_connection.hpp` - PlainConnection class (raw socket, no TLS)
- `src/net/plain_connection.cpp` - PlainConnection implementation
- `tests/net/test_plain_connection.cpp` - PlainConnection tests

**Files to modify:**
- `include/hpactor/net/tls_connection.hpp` - Make inherit from `Connection` instead of just `enable_shared_from_this`
- `src/net/tls_connection.cpp` - Update constructor to call `Connection` base
- `include/hpactor/net/connection_pool.hpp` - Add `use_tls`, change to `ConnectionPtr`, make `on_connection_ready/error` public
- `src/net/connection_pool.cpp` - Remove `create_connection()`, add `add_connection()`, update to `ConnectionPtr`
- `include/hpactor/net/tcp_transport.hpp` - Change `connections_` map to `ConnectionPtr`
- `src/net/tcp_transport.cpp` - Fix client-side fd assignment, use connection factory
- `include/hpactor/net/tls_connection.hpp` - Add `set_fd()` method
- `src/net/tls_connection.cpp` - Implement `set_fd()`
- `tests/net/test_tls_connection.cpp` - Ensure TLS tests still work
- `tests/net/test_connection_pool.cpp` - Ensure pool tests work with default plain config

---

## Architecture Summary

**Key changes:**

1. `TlsConnection` now inherits from `Connection` (was: `enable_shared_from_this` only)
2. `PlainConnection` - new class, inherits from `Connection`, raw socket without TLS
3. `TcpTransport::connections_` changes from `unordered_map<int, TlsConnectionPtr>` to `unordered_map<int, ConnectionPtr>`
4. `ConnectionPool::active_connections_` changes from `vector<TlsConnectionPtr>` to `vector<ConnectionPtr>`
5. `ConnectionPool::on_connection_ready()` and `on_connection_error()` become **public** so TcpTransport can call them
6. `TlsConnection::set_fd(int fd)` - new method to assign connected socket fd to client connections
7. `PoolConfig::use_tls` - new flag, defaults to `false`

**Before:**
```
TcpTransport
├── connections_: unordered_map<int, TlsConnectionPtr>  // Only TlsConnection (not Connection!)
├── connect(): creates TlsConnection with fd=-1 (broken)
└── handle_accept(): creates TlsConnection (works)

ConnectionPool : public Connection
├── active_connections_: vector<TlsConnectionPtr>
└── create_connection(): creates TLS connection only

TlsConnection : public enable_shared_from_this  // NOT Connection!
PlainConnection: (didn't exist)
```

**After:**
```
TcpTransport
├── connections_: unordered_map<int, ConnectionPtr>  // Both types inherit Connection
├── connect(): creates socket, then PlainConnection or TlsConnection with set_fd()
└── handle_accept(): creates PlainConnection or TlsConnection based on use_tls

Connection : public std::enable_shared_from_this<Connection>
├── TlsConnection : public Connection
└── PlainConnection : public Connection

ConnectionPool : public Connection
├── active_connections_: vector<ConnectionPtr>
├── add_connection(): adds externally-created connection
├── on_connection_ready(ConnectionPtr): public, called by TcpTransport
└── on_connection_error(ConnectionPtr, error): public, called by TcpTransport
```

---

## Task 0: Make TlsConnection inherit from Connection

**CRITICAL PREREQUISITE:** `TlsConnection` currently inherits from `std::enable_shared_from_this<TlsConnection>`, NOT from `Connection`. For the polymorphic design to work, `TlsConnection` must inherit from `Connection`.

**Files:**
- Modify: `include/hpactor/net/tls_connection.hpp`
- Modify: `include/hpactor/net/transport.hpp` (may need to forward declare)

- [ ] **Step 1: Change TlsConnection inheritance**

In `include/hpactor/net/tls_connection.hpp`:

```cpp
// FROM:
class TlsConnection : public std::enable_shared_from_this<TlsConnection> {

// TO:
class TlsConnection : public Connection, public std::enable_shared_from_this<TlsConnection> {
```

- [ ] **Step 2: Fix constructor to call Connection base**

The `Connection` base class has a constructor `Connection(NodeId remote_node)`. Update `TlsConnection` constructor:

```cpp
// FROM:
TlsConnection::TlsConnection(NodeId remote_node_id,
                             TlsContext* tls_context,
                             EventLoop* loop,
                             int socket_fd)
    : remote_node_id_(remote_node_id),
      tls_context_(tls_context),
      loop_(loop),
      fd_(socket_fd) {

// TO:
TlsConnection::TlsConnection(NodeId remote_node_id,
                             TlsContext* tls_context,
                             EventLoop* loop,
                             int socket_fd)
    : Connection(remote_node_id),  // Call base constructor
      remote_node_id_(remote_node_id),
      tls_context_(tls_context),
      loop_(loop),
      fd_(socket_fd) {
```

Note: We still keep `remote_node_id_` as a separate member since `Connection::remote_node_` is private and we need to override the getter for `fd()`.

- [ ] **Step 3: Implement required Connection virtual methods**

`Connection` requires:
- `send(const bytes& data) = 0` - add this to TlsConnection
- `close() = 0` - TlsConnection already has `close()`
- `handle_read(const bytes& data)` - TlsConnection already has this

Add to `TlsConnection`:
```cpp
void send(const bytes& data) override {
    // TlsConnection::send() is the same - just make it override
    send(data);  // Calls existing send method
}
```

Wait, TlsConnection's `send()` takes bytes and is already implemented. Just add `override` specifier if needed.

Actually, `Connection` base class has `virtual void send(const bytes& data) = 0;`. TlsConnection already has `void send(const bytes& frame_data)` which can serve as the implementation. The signature matches.

- [ ] **Step 4: Build to verify**

Run: `ninja -C build 2>&1 | tail -20`
Expected: Build succeeds (may have errors - fix them)

**Common issues:**
- Virtual function override warnings - add `override` where needed
- Constructor order - Connection base must be initialized before members

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/net/tls_connection.hpp
git commit -m "refactor(tls): make TlsConnection inherit from Connection base"
```

---

## Task 1: Add use_tls flag to PoolConfig

**Files:**
- Modify: `include/hpactor/net/connection_pool.hpp`

- [ ] **Step 1: Add `use_tls` member to PoolConfig**

In `include/hpactor/net/connection_pool.hpp`, find `PoolConfig` struct and add:

```cpp
struct PoolConfig {
    // ... existing members ...
    bool use_tls = false;  // Default to plain text
};
```

- [ ] **Step 2: Build to verify**

Run: `ninja -C build 2>&1 | tail -10`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/connection_pool.hpp
git commit -m "feat(net): add use_tls flag to PoolConfig for optional TLS"
```

---

## Task 2: Add set_fd() to TlsConnection

**Files:**
- Modify: `include/hpactor/net/tls_connection.hpp`
- Modify: `src/net/tls_connection.cpp`

- [ ] **Step 1: Add set_fd() declaration**

In `include/hpactor/net/tls_connection.hpp`, add after `create_server()`:

```cpp
// Set file descriptor (for connected client sockets after TCP handshake)
void set_fd(int fd);
```

- [ ] **Step 2: Implement set_fd()**

In `src/net/tls_connection.cpp`, add before `start_client_handshake()`:

```cpp
void TlsConnection::set_fd(int fd) {
    fd_ = fd;
    // Register fd with event loop for read events
    if (loop_ && fd >= 0) {
        loop_->add_fd(fd, EventLoop::Event::Read);
    }
}
```

- [ ] **Step 3: Build to verify**

Run: `ninja -C build 2>&1 | tail -10`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/tls_connection.hpp src/net/tls_connection.cpp
git commit -m "feat(tls): add set_fd() to assign connected socket fd to client"
```

---

## Task 3: Create PlainConnection class

**Files:**
- Create: `include/hpactor/net/plain_connection.hpp`
- Create: `src/net/plain_connection.cpp`

- [ ] **Step 1: Create PlainConnection header**

Create `include/hpactor/net/plain_connection.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <hpactor/net/transport.hpp>
#include <hpactor/net/event_loop.hpp>

#include <functional>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// PlainConnection - raw socket connection without TLS
// -----------------------------------------------------------------------------
// A simple connection wrapper for plain TCP sockets without encryption.
// Used when use_tls=false in PoolConfig.
// -----------------------------------------------------------------------------

class PlainConnection : public Connection, public std::enable_shared_from_this<PlainConnection> {
public:
    // Create client-side connection with existing connected fd
    static PlainConnectionPtr create_client(int fd,
                                           NodeId remote_node_id,
                                           EventLoop* loop);

    // Create server-side connection (from accepted socket)
    static PlainConnectionPtr create_server(int fd,
                                           NodeId remote_node_id,
                                           EventLoop* loop);

    ~PlainConnection();

    // Non-copyable
    PlainConnection(const PlainConnection&) = delete;
    PlainConnection& operator=(const PlainConnection&) = delete;

    // Getters
    NodeId remote_node_id() const { return remote_node_id_; }
    ConnectionState state() const { return state_; }
    int fd() const { return fd_; }

    // Set callbacks
    void set_ready_handler(connection_ready_handler handler);
    void set_frame_handler(frame_handler handler);
    void set_error_handler(connection_error_handler handler);
    void set_send_completion_handler(std::function<void(int result)> handler);

    // Send raw frame data
    void send(const bytes& frame_data);

    // Close connection
    void close();

    // Handle incoming data (for framing)
    void handle_read(const bytes& data);

    // Handle send completion (called by EventLoop)
    void handle_send_completion(int result);

private:
    PlainConnection(NodeId remote_node_id, EventLoop* loop, int fd);

    void set_state(ConnectionState new_state);

    // Send raw bytes on socket
    void send_raw(const bytes& data);

    // Flush write buffer
    void flush_write_buffer();

    NodeId remote_node_id_ = 0;
    EventLoop* loop_ = nullptr;
    int fd_ = -1;

    ConnectionState state_ = ConnectionState::Disconnected;

    // Read buffer
    bytes read_buffer_;

    // Write buffer
    bytes write_buffer_;

    // True while async send is in progress
    bool is_sending_ = false;

    // Callbacks
    connection_ready_handler ready_handler_;
    frame_handler frame_handler_;
    connection_error_handler error_handler_;
    std::function<void(int result)> send_completion_handler_;
};

// Connection pointer type
using PlainConnectionPtr = std::shared_ptr<PlainConnection>;

} // namespace net
} // namespace hpactor
```

- [ ] **Step 2: Create PlainConnection implementation**

Create `src/net/plain_connection.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/net/plain_connection.hpp>

#include <hpactor/net/event_loop.hpp>

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace hpactor {

namespace net {

PlainConnection::PlainConnection(NodeId remote_node_id,
                                EventLoop* loop,
                                int socket_fd)
    : remote_node_id_(remote_node_id),
      loop_(loop),
      fd_(socket_fd) {}

PlainConnection::~PlainConnection() {
    close();
}

PlainConnectionPtr PlainConnection::create_client(int fd,
                                                   NodeId remote_node_id,
                                                   EventLoop* loop) {
    auto conn = std::shared_ptr<PlainConnection>(
        new PlainConnection(remote_node_id, loop, fd));
    conn->set_state(ConnectionState::Connected);
    conn->is_sending_ = false;
    return conn;
}

PlainConnectionPtr PlainConnection::create_server(int fd,
                                                   NodeId remote_node_id,
                                                   EventLoop* loop) {
    auto conn = std::shared_ptr<PlainConnection>(
        new PlainConnection(remote_node_id, loop, fd));
    conn->set_state(ConnectionState::Connected);
    conn->is_sending_ = false;

    // Register fd with event loop for read events
    if (loop && fd >= 0) {
        loop->add_fd(fd, EventLoop::Event::Read);
    }

    return conn;
}

void PlainConnection::set_ready_handler(connection_ready_handler handler) {
    ready_handler_ = std::move(handler);
}

void PlainConnection::set_frame_handler(frame_handler handler) {
    frame_handler_ = std::move(handler);
}

void PlainConnection::set_error_handler(connection_error_handler handler) {
    error_handler_ = std::move(handler);
}

void PlainConnection::set_send_completion_handler(std::function<void(int result)> handler) {
    send_completion_handler_ = std::move(handler);
}

void PlainConnection::send(const bytes& frame_data) {
    if (state_ != ConnectionState::Connected) {
        return;
    }
    send_raw(frame_data);
}

void PlainConnection::close() {
    if (fd_ >= 0) {
        if (loop_) {
            loop_->remove_fd(fd_);
        }
        ::close(fd_);
        fd_ = -1;
    }
    set_state(ConnectionState::Disconnected);
}

void PlainConnection::handle_read(const bytes& data) {
    read_buffer_.insert(read_buffer_.end(), data.begin(), data.end());

    // Process complete frames
    // Simple framing: 4-byte length header
    while (read_buffer_.size() >= 4) {
        size_t frame_len = (static_cast<size_t>(read_buffer_[0]) << 24) |
                          (static_cast<size_t>(read_buffer_[1]) << 16) |
                          (static_cast<size_t>(read_buffer_[2]) << 8) |
                          static_cast<size_t>(read_buffer_[3]);

        if (read_buffer_.size() < 4 + frame_len) {
            break;  // Wait for more data
        }

        bytes frame(read_buffer_.begin() + 4,
                    read_buffer_.begin() + 4 + frame_len);
        read_buffer_.erase(read_buffer_.begin(),
                          read_buffer_.begin() + 4 + frame_len);

        if (frame_handler_) {
            frame_handler_(frame);
        }
    }
}

void PlainConnection::send_raw(const bytes& data) {
    if (fd_ < 0 || !loop_) return;

    // Append data to write buffer
    write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());

    // If already sending, wait for completion
    if (is_sending_) return;

    flush_write_buffer();
}

void PlainConnection::flush_write_buffer() {
    if (fd_ < 0 || loop_ == nullptr || write_buffer_.empty()) {
        return;
    }

    is_sending_ = true;

    struct iovec iov;
    iov.iov_base = write_buffer_.data();
    iov.iov_len = write_buffer_.size();

    // Use async_send - completion will be delivered via loop's completion callback
    loop_->backend()->async_send(fd_, &iov, 1, ActorId(0), static_cast<uint32_t>(OpType::Send));
}

void PlainConnection::handle_send_completion(int result) {
    if (send_completion_handler_) {
        send_completion_handler_(result);
    }
    is_sending_ = false;

    if (result < 0) {
        // Send error - close connection
        set_state(ConnectionState::Error);
        if (error_handler_) {
            error_handler_(shared_from_this(), error{});
        }
        return;
    }

    // Remove sent bytes from write buffer
    if (static_cast<size_t>(result) >= write_buffer_.size()) {
        write_buffer_.clear();
    } else {
        write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + result);
    }

    // If more data to send, continue flushing
    if (!write_buffer_.empty()) {
        flush_write_buffer();
    }
}

void PlainConnection::set_state(ConnectionState new_state) {
    state_ = new_state;
}

} // namespace net
} // namespace hpactor
```

- [ ] **Step 3: Build to verify**

Run: `ninja -C build 2>&1 | tail -10`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/plain_connection.hpp src/net/plain_connection.cpp
git commit -m "feat(net): add PlainConnection for non-TLS communication"
```

---

## Task 4: Update TcpTransport connections_ map to use ConnectionPtr

**Files:**
- Modify: `include/hpactor/net/tcp_transport.hpp`
- Modify: `src/net/tcp_transport.cpp`

- [ ] **Step 1: Change connections_ map type**

In `include/hpactor/net/tcp_transport.hpp`, change:

```cpp
// From:
std::unordered_map<int, TlsConnectionPtr> connections_;

// To:
std::unordered_map<int, ConnectionPtr> connections_;
```

Add `#include <hpactor/net/plain_connection.hpp>` if not already present.

- [ ] **Step 2: Update completion callback to use ConnectionPtr**

In `src/net/tcp_transport.cpp`, the completion callback accesses `connections_` via fd lookup:

```cpp
completion_callback_ = [this](OpCompletion c) {
    if (c.type == OpType::Send) {
        auto it = connections_.find(c.fd);
        if (it != connections_.end()) {
            // Use Connection interface - need to cast to connection type that has handle_send_completion
            // Both PlainConnection and TlsConnection have handle_send_completion
            // Since Connection base doesn't have it, we need a different approach
        }
    }
};
```

**Problem:** `Connection` base class doesn't have `handle_send_completion()`. Only `PlainConnection` and `TlsConnection` have it.

**Solution:** The completion callback cannot use polymorphism for `handle_send_completion()`. Instead, we need to ensure `handle_send_completion()` is part of the `Connection` base class, OR we store connections in the map with their actual type and use a helper.

**Decision:** Add `handle_send_completion()` to `Connection` base class with a default implementation (no-op or error). Both `PlainConnection` and `TlsConnection` override it.

- [ ] **Step 3: Add handle_send_completion to Connection base class**

In `include/hpactor/net/transport.hpp`, add to `Connection` class:

```cpp
// Handle send completion (for both PlainConnection and TlsConnection)
virtual void handle_send_completion(int result) {}
```

- [ ] **Step 4: Override in TlsConnection**

In `include/hpactor/net/tls_connection.hpp`, ensure `handle_send_completion()` is in public section:

```cpp
public:
    // ... existing public methods ...
    void handle_send_completion(int result) override;
```

- [ ] **Step 5: Override in PlainConnection**

In `include/hpactor/net/plain_connection.hpp`, add override:

```cpp
public:
    // ... existing public methods ...
    void handle_send_completion(int result) override;
```

And in `src/net/plain_connection.cpp`, it's already implemented.

- [ ] **Step 6: Update completion callback to call via base pointer**

In `src/net/tcp_transport.cpp`:

```cpp
completion_callback_ = [this](OpCompletion c) {
    if (c.type == OpType::Send) {
        auto it = connections_.find(c.fd);
        if (it != connections_.end()) {
            it->second->handle_send_completion(c.result);
        }
    }
};
```

- [ ] **Step 7: Update register_connection/unregister_connection signatures**

In `include/hpactor/net/tcp_transport.hpp`:

```cpp
// From:
void register_connection(TlsConnectionPtr conn, int fd);
void unregister_connection(int fd);

// To:
void register_connection(ConnectionPtr conn, int fd);
```

In `src/net/tcp_transport.cpp`:

```cpp
// Update implementations to use ConnectionPtr
void TcpTransport::register_connection(ConnectionPtr conn, int fd) {
    connections_[fd] = conn;
}
```

- [ ] **Step 8: Build to verify**

Run: `ninja -C build 2>&1 | tail -20`
Expected: Build succeeds (may have errors - fix them)

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/net/tcp_transport.hpp src/net/tcp_transport.cpp include/hpactor/net/transport.hpp
git commit -m "refactor(net): update TcpTransport connections_ to use ConnectionPtr base"
```

---

## Task 5: Update ConnectionPool to use ConnectionPtr and add add_connection()

**Files:**
- Modify: `include/hpactor/net/connection_pool.hpp`
- Modify: `src/net/connection_pool.cpp`

- [ ] **Step 1: Add #include for PlainConnection**

In `include/hpactor/net/connection_pool.hpp`, add after existing includes:

```cpp
#include <hpactor/net/plain_connection.hpp>
```

- [ ] **Step 2: Change active_connections_ to ConnectionPtr**

In `include/hpactor/net/connection_pool.hpp`:

```cpp
// From:
std::vector<TlsConnectionPtr> active_connections_;

// To:
std::vector<ConnectionPtr> active_connections_;
```

- [ ] **Step 3: Change get_connection() return type**

```cpp
// From:
TlsConnectionPtr get_connection();

// To:
ConnectionPtr get_connection();
```

- [ ] **Step 4: Make on_connection_ready and on_connection_error public**

Move these from private to public section:

```cpp
public:
    // Called by TcpTransport when connection becomes ready
    void on_connection_ready(ConnectionPtr conn);

    // Called by TcpTransport when connection has error
    void on_connection_error(ConnectionPtr conn, const error& err);
```

- [ ] **Step 5: Remove create_connection() - it's being replaced**

Remove the private `create_connection()` method entirely. TcpTransport will create connections and pass them via `add_connection()`.

- [ ] **Step 6: Add add_connection() method**

In public section:

```cpp
// Add an externally-created connection to the pool
void add_connection(ConnectionPtr conn);
```

- [ ] **Step 7: Update connection_pool.cpp**

In `src/net/connection_pool.cpp`:

1. Update `get_connection()` return type:

```cpp
ConnectionPtr ConnectionPool::get_connection() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_connections_.empty()) {
        return nullptr;
    }
    auto index = next_index_.fetch_add(1) % active_connections_.size();
    return active_connections_[index];
}
```

2. Add `add_connection()`:

```cpp
void ConnectionPool::add_connection(ConnectionPtr conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_connections_.push_back(conn);
}
```

3. Update `on_connection_ready()` and `on_connection_error()` signatures to take `ConnectionPtr`:

```cpp
void ConnectionPool::on_connection_ready(ConnectionPtr conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connections_.push_back(conn);
    }
    connecting_.store(false);
    flush_pending();
}

void ConnectionPool::on_connection_error(ConnectionPtr conn, const error& err) {
    (void)err;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connections_.erase(
            std::remove(active_connections_.begin(), active_connections_.end(), conn),
            active_connections_.end());
    }
    schedule_reconnect();
}
```

4. Remove `create_connection()` implementation entirely.

5. Update `send()` - it no longer calls `create_connection()`:

```cpp
void ConnectionPool::send(const ActorAddress& target, const bytes& encoded) {
    if (shutting_down_.load()) {
        return;
    }

    ConnectionPtr conn = get_connection();
    if (conn) {
        conn->send(encoded);
        return;
    }

    // No connection available, queue pending
    if (!add_pending(target, encoded)) {
        return;  // Queue full
    }

    // Connection creation is handled by TcpTransport, not ConnectionPool
}
```

- [ ] **Step 8: Build to verify**

Run: `ninja -C build 2>&1 | tail -20`
Expected: Build succeeds (may have errors - fix them)

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/net/connection_pool.hpp src/net/connection_pool.cpp
git commit -m "refactor(pool): use ConnectionPtr and add add_connection() for external creation"
```

---

## Task 6: Fix TcpTransport::connect() to create correct connection type with proper fd

**Files:**
- Modify: `src/net/tcp_transport.cpp`

- [ ] **Step 1: Rewrite connect() to create socket and correct connection type**

Replace the current `TcpTransport::connect()` implementation:

```cpp
ConnectionPtr TcpTransport::connect(NodeId remote_node,
                                   const std::string& /*host*/,
                                   uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return nullptr;
    }

    // Set TCP_NODELAY
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Resolve host (simple version - no DNS)
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int result = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return nullptr;
    }

    // Get or create the pool
    auto pool = get_or_create_pool(remote_node);

    ConnectionPtr conn;
    if (pool_config_.use_tls) {
        // Create TLS connection
        auto tls_conn = TlsConnection::create_client(remote_node, &tls_context_, &loop_);
        tls_conn->set_fd(fd);  // Assign connected socket
        tls_conn->set_ready_handler([pool](TlsConnectionPtr c) {
            pool->on_connection_ready(std::static_pointer_cast<Connection>(c));
        });
        tls_conn->set_error_handler([pool](TlsConnectionPtr c, const error& e) {
            pool->on_connection_error(std::static_pointer_cast<Connection>(c), e);
        });
        tls_conn->set_frame_handler([pool](const bytes& data) {
            pool->on_frame_received(data);
        });
        conn = tls_conn;
        tls_conn->start_client_handshake();
    } else {
        // Create plain connection with connected fd
        auto plain_conn = PlainConnection::create_client(fd, remote_node, &loop_);
        plain_conn->set_ready_handler([pool](ConnectionPtr c) {
            pool->on_connection_ready(c);
        });
        plain_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
            pool->on_connection_error(c, e);
        });
        plain_conn->set_frame_handler([pool](const bytes& data) {
            pool->on_frame_received(data);
        });
        conn = plain_conn;
    }

    // Add to pool and track by fd for completion routing
    pool->add_connection(conn);
    register_connection(conn, fd);

    return pool;
}
```

Note: We pass `pool` by value in the lambdas (shared_ptr copies), which is correct for lifetime management.

- [ ] **Step 2: Build to verify**

Run: `ninja -C build 2>&1 | tail -20`
Expected: Build succeeds (may have errors - fix them)

- [ ] **Step 3: Commit**

```bash
git add src/net/tcp_transport.cpp
git commit -m "fix(tcp): create correct connection type in connect() with proper fd"
```

---

## Task 7: Update TcpTransport::handle_accept() to support plain connections

**Files:**
- Modify: `src/net/tcp_transport.cpp`

- [ ] **Step 1: Update handle_accept to create correct connection type**

Replace `handle_accept()` implementation:

```cpp
void TcpTransport::handle_accept(int client_fd) {
    ConnectionPtr conn;
    if (pool_config_.use_tls) {
        conn = TlsConnection::create_server(client_fd, 0, &tls_context_, &loop_);
    } else {
        conn = PlainConnection::create_server(client_fd, 0, &loop_);
    }
    register_connection(conn, client_fd);
}
```

Note: The completion routing works because both connection types have `handle_send_completion()` (now in base `Connection` class).

- [ ] **Step 2: Build to verify**

Run: `ninja -C build 2>&1 | tail -20`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add src/net/tcp_transport.cpp
git commit -m "feat(tcp): support plain connections in handle_accept"
```

---

## Task 8: Create tests for PlainConnection

**Files:**
- Create: `tests/net/test_plain_connection.cpp`

- [ ] **Step 1: Write basic PlainConnection tests**

```cpp
// Copyright 2026 HPActor Contributors
//
// Test PlainConnection basic functionality
//

#include <hpactor/net/plain_connection.hpp>
#include <hpactor/net/event_loop.hpp>

#include <cassert>
#include <sys/socket.h>
#include <fcntl.h>

using namespace hpactor;
using namespace hpactor::net;

std::pair<int, int> create_socket_pair() {
    int sv[2];
    int ret = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(ret == 0);
    int flags = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(sv[1], F_GETFL, 0);
    fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);
    return {sv[0], sv[1]};
}

int main() {
    std::cout << "Test: PlainConnection creation" << std::endl;
    EventLoop loop;
    auto [fd1, fd2] = create_socket_pair();

    // Test create_server
    auto conn = PlainConnection::create_server(fd1, 42, &loop);
    assert(conn != nullptr);
    assert(conn->remote_node_id() == 42);
    assert(conn->state() == ConnectionState::Connected);
    assert(conn->fd() == fd1);

    // Test create_client
    auto client = PlainConnection::create_client(fd2, 100, &loop);
    assert(client != nullptr);
    assert(client->remote_node_id() == 100);
    assert(client->state() == ConnectionState::Connected);

    // Test close
    conn->close();
    assert(conn->state() == ConnectionState::Disconnected);
    assert(conn->fd() == -1);

    ::close(fd1);
    ::close(fd2);

    std::cout << "All PlainConnection tests passed" << std::endl;
    return 0;
}
```

- [ ] **Step 2: Build and run tests**

Add to CMakeLists.txt and run.

- [ ] **Step 3: Commit**

---

## Task 9: Verify all tests pass

- [ ] **Step 1: Run all tests**

Run: `ctest --output-on-failure 2>&1 | tail -30`
Expected: All tests pass

- [ ] **Step 2: Run TLS-specific tests**

Run: `ctest -R tls --output-on-failure 2>&1`
Expected: All TLS tests pass

- [ ] **Step 3: Run plain connection tests**

Run: `ctest -R plain --output-on-failure 2>&1`
Expected: All plain connection tests pass

---

## Summary

After completing all tasks:
1. `PoolConfig::use_tls` flag defaults to `false` (plain text)
2. `PlainConnection` class provides raw socket communication without TLS
3. `Connection::handle_send_completion()` added to base class for polymorphic completion routing
4. `TlsConnection::set_fd()` added to assign connected socket fd to client
5. `TcpTransport::connections_` uses `ConnectionPtr` for both connection types
6. `ConnectionPool` uses `ConnectionPtr`, `add_connection()` for external creation, `on_connection_ready/error` are public
7. `TcpTransport::connect()` creates socket, connects, creates correct connection type
8. `TcpTransport::handle_accept()` creates correct connection type based on `use_tls`
9. All existing tests pass, new PlainConnection tests verify plain text

---

## Architecture After Changes

```
TcpTransport
├── connections_: unordered_map<int, ConnectionPtr>
├── pool_config_.use_tls = false (default)
│   └── connect() → creates PlainConnection
└── pool_config_.use_tls = true
    └── connect() → creates TlsConnection

Both connection types:
- Inherit from Connection
- Have handle_send_completion() (base virtual or overridden)
- Have set_ready_handler(), set_frame_handler(), set_error_handler()

ConnectionPool
├── active_connections_: vector<ConnectionPtr>
├── add_connection(ConnectionPtr) → add external connection
├── on_connection_ready(ConnectionPtr) → public, called by TcpTransport
└── on_connection_error(ConnectionPtr, error) → public, called by TcpTransport
```
