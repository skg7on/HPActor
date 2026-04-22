# Optional TLS / Plain Text Connection Design Spec

**Goal:** Make TLS optional in TcpTransport/ConnectionPool. Default to plain text communication without TLS. Allow TLS to be enabled via configuration when secure communication is needed.

**Architecture:** Introduce `PlainConnection` for raw socket communication without TLS. Both `PlainConnection` and `TlsConnection` inherit from `Connection` base class. ConnectionPool manages both types via `ConnectionPtr`. Add `use_tls` flag to `PoolConfig`.

**Tech Stack:** C++20, EventLoop, KqueueBackend/EpollBackend, OpenSSL (when TLS enabled)

---

## Problem Statement

Currently `TlsConnection` inherits from `std::enable_shared_from_this<TlsConnection>`, NOT from `Connection`. This prevents polymorphic management of connections via `ConnectionPtr`.

Additionally:
- `TlsConnection::create_client()` creates a connection with `fd=-1` that never gets updated
- `TlsConnection` is always required, even when TLS is not needed
- `ConnectionPool` creates TLS connections internally with no way to inject plain connections

---

## Design Decisions

### 1. Connection Inheritance Hierarchy

**Before:**
```
Connection : public std::enable_shared_from_this<Connection>
    (no implementations of send/close - these are abstract)

TlsConnection : public std::enable_shared_from_this<TlsConnection>  // NOT Connection!
PlainConnection: (didn't exist)
```

**After:**
```
Connection : public std::enable_shared_from_this<Connection>
├── TlsConnection : public Connection, public std::enable_shared_from_this<TlsConnection>
└── PlainConnection : public Connection, public std::enable_shared_from_this<PlainConnection>
```

Both `TlsConnection` and `PlainConnection` implement the `Connection` interface:
- `send(const bytes& data)` - send frame data
- `close()` - close the connection
- `handle_send_completion(int result)` - called by EventLoop on async_send completion

### 2. Connection Pointer Types

Callback types defined at `Connection` level for polymorphic usage:

```cpp
using connection_ready_handler = std::function<void(ConnectionPtr)>;
using frame_handler = std::function<void(const bytes&)>;
using connection_error_handler = std::function<void(ConnectionPtr, const error&)>;
```

### 3. PlainConnection Class

A minimal connection class for raw TCP sockets without TLS:

- Inherits from `Connection`
- Simple 4-byte length-prefixed framing (vs. TLS record layer)
- No handshake - immediately ready for frame exchange
- Used when `PoolConfig::use_tls = false`

### 4. Connection Pool Changes

**Before:**
```cpp
class ConnectionPool : public Connection {
    std::vector<TlsConnectionPtr> active_connections_;
    void create_connection();  // Creates TLS internally
    void on_connection_ready(TlsConnectionPtr conn);  // Private
    void on_connection_error(TlsConnectionPtr conn, const error& err);  // Private
};
```

**After:**
```cpp
class ConnectionPool : public Connection {
    std::vector<ConnectionPtr> active_connections_;
    void add_connection(ConnectionPtr conn);  // Add externally-created connection
    void on_connection_ready(ConnectionPtr conn);  // Public - called by TcpTransport
    void on_connection_error(ConnectionPtr conn, const error& err);  // Public - called by TcpTransport
    void on_frame_received(const bytes& frame_data);  // Public - called by connection's frame handler
};
```

### 5. TcpTransport Changes

**Before:**
```cpp
class TcpTransport {
    std::unordered_map<int, TlsConnectionPtr> connections_;  // TlsConnectionPtr only
    // connect() creates TlsConnection but doesn't properly set fd
};

void TcpTransport::connect(NodeId remote_node, const std::string& host, uint16_t port) {
    int fd = ::socket(...);
    ::connect(fd, ...);
    auto conn = TlsConnection::create_client(remote_node, &tls_context_, &loop_);
    conn->start_client_handshake();  // fd is still -1! (BUG)
    return pool;
}
```

**After:**
```cpp
class TcpTransport {
    std::unordered_map<int, ConnectionPtr> connections_;  // ConnectionPtr base type
};

void TcpTransport::connect(NodeId remote_node, const std::string& host, uint16_t port) {
    int fd = ::socket(...);
    ::connect(fd, ...);
    auto pool = get_or_create_pool(remote_node);

    ConnectionPtr conn;
    if (pool_config_.use_tls) {
        auto tls_conn = TlsConnection::create_client(remote_node, &tls_context_, &loop_);
        tls_conn->set_fd(fd);  // Assign connected socket!
        // Set up handlers...
        conn = tls_conn;
        tls_conn->start_client_handshake();
    } else {
        conn = PlainConnection::create_client(fd, remote_node, &loop_);
        // Set up handlers...
    }

    pool->add_connection(conn);
    register_connection(conn, fd);  // Track by fd for completion routing
    return pool;
}
```

### 6. TlsConnection::set_fd() Method

Client-side TLS connections need `set_fd()` because:
1. `create_client()` is called before socket is connected
2. TCP connect is done in `TcpTransport::connect()`
3. Only after connect succeeds do we have a valid fd

```cpp
void TlsConnection::set_fd(int fd) {
    fd_ = fd;
    if (loop_ && fd >= 0) {
        loop_->add_fd(fd, EventLoop::Event::Read);
    }
}
```

### 7. PoolConfig::use_tls Flag

```cpp
struct PoolConfig {
    size_t min_connections = 1;
    size_t max_connections = 4;
    size_t max_pending = 1000;
    size_t max_attempts = 5;
    std::chrono::milliseconds initial_backoff{1000};
    std::chrono::milliseconds max_backoff{16000};
    bool use_tls = false;  // Default to plain text
};
```

---

## Before/After Architecture Diagrams

### Before
```
TcpTransport
├── connections_: unordered_map<int, TlsConnectionPtr>  // Only TlsConnection (not Connection!)
├── connect(): creates TlsConnection with fd=-1 (broken)
└── handle_accept(): creates TlsConnection (works)

ConnectionPool : public Connection
├── active_connections_: vector<TlsConnectionPtr>
└── create_connection(): creates TLS connection only

TlsConnection : public std::enable_shared_from_this  // NOT Connection!
PlainConnection: (didn't exist)
```

### After
```
TcpTransport
├── connections_: unordered_map<int, ConnectionPtr>  // Both connection types
├── pool_config_.use_tls = false (default)
│   └── connect() → creates PlainConnection
└── pool_config_.use_tls = true
    └── connect() → creates TlsConnection

Connection : public std::enable_shared_from_this<Connection>
├── TlsConnection : public Connection
└── PlainConnection : public Connection

ConnectionPool : public Connection
├── active_connections_: vector<ConnectionPtr>
├── add_connection(ConnectionPtr) → add external connection
├── on_connection_ready(ConnectionPtr) → public, called by TcpTransport
├── on_connection_error(ConnectionPtr, error) → public, called by TcpTransport
└── on_frame_received(bytes) → public, called by connection's frame handler
```

---

## Key Benefits

1. **Plain text by default** - No TLS overhead when security not needed
2. **Polymorphic connection management** - Single `ConnectionPtr` type for both
3. **Proper fd assignment** - Client connections get correct fd after TCP handshake
4. **External connection creation** - TcpTransport creates connections, pool manages them
5. **Event loop integration** - Completion routing via `connections_` map works for both types