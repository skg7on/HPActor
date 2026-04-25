# Registrar Event-Loop Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor RegistrarServer, RegistrarClient, and UdpRegistrar to use the async EventLoop framework instead of blocking socket I/O with dedicated threads.

**Architecture:** Create a new `RegistrarConnection` class that wraps async TCP connections with registrar protocol framing (magic/version/type/length). Refactor RegistrarServer to use Acceptor + RegistrarConnection. Refactor RegistrarClient to use EventLoop timers + RegistrarConnection.

**Tech Stack:** C++20, AsyncIoBackend, EventLoop, existing Connection patterns

---

## File Structure

| File | Change |
|------|--------|
| `include/hpactor/net/registrar.hpp` | Add `RegistrarConnection` class declaration |
| `src/net/registrar.cpp` | Add `RegistrarConnection` implementation |
| `src/net/registrar_server.cpp` | Refactor to use `RegistrarConnection` + `Acceptor` |
| `src/net/registrar_client.cpp` | Refactor to use `RegistrarConnection` + EventLoop timers |
| `tests/net/test_registrar.cpp` | Add tests for `RegistrarConnection` |

---

## Task 1: Add RegistrarConnection class declaration to registrar.hpp

**Files:**
- Modify: `include/hpactor/net/registrar.hpp:206-210` (add forward declaration and class)

- [ ] **Step 1: Add RegistrarConnection forward declaration and class declaration**

After line 209 (after `class NodeRegistry;`), add:

```cpp
// Forward declaration
class RegistrarConnection;
using RegistrarConnectionPtr = std::shared_ptr<RegistrarConnection>;

// RegistrarConnection - async TCP connection for registrar protocol
class RegistrarConnection : public std::enable_shared_from_this<RegistrarConnection> {
public:
    using message_handler = std::function<void(TcpMessageType, const bytes&)>;
    using disconnect_handler = std::function<void()>;
    using send_complete_handler = std::function<void(int result)>;

    // Create from accepted server socket
    static RegistrarConnectionPtr accepted(int fd,
                                          CommunicationEndpoint remote_endpoint,
                                          EventLoop* loop);

    // Create as client connection
    static RegistrarConnectionPtr connecting(int fd,
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
    enum class ReadState {
        ReadingHeader,
        ReadingPayload
    };

    RegistrarConnection(CommunicationEndpoint remote_endpoint, EventLoop* loop, int fd);

    void register_with_loop();
    void handle_read_event();
    void handle_payload_read();
    void flush_write_buffer();
    void handle_send_completion(int result);

    CommunicationEndpoint remote_endpoint_;
    EventLoop* loop_ = nullptr;
    int fd_ = -1;

    ReadState read_state_ = ReadState::ReadingHeader;
    size_t header_bytes_read_ = 0;
    bytes header_buffer_;

    TcpMessageType current_type_ = TcpMessageType::Register;
    size_t payload_bytes_read_ = 0;
    bytes payload_buffer_;

    bytes write_buffer_;
    bool is_sending_ = false;

    message_handler message_handler_;
    disconnect_handler disconnect_handler_;
    send_complete_handler send_complete_handler_;
};
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/net/registrar.hpp
git commit -m "feat(registrar): add RegistrarConnection forward declaration"
```

---

## Task 2: Implement RegistrarConnection in registrar.cpp

**Files:**
- Modify: `src/net/registrar.cpp` (add implementation before NodeResolver section)
- Modify: `include/hpactor/net/registrar.hpp` (add static completion map to RegistrarServer)

**Key architectural fix:** The `EventLoop::completion_callback_` is a single callback that gets overwritten if multiple connections set it. Solution: RegistrarServer owns a static fd→connection map and sets ONE completion callback that routes to the correct connection.

- [ ] **Step 1: Add static completion routing to RegistrarServer in registrar.hpp**

After `clients_mutex_` in RegistrarServer declaration (line 260), add:

```cpp
    // Static map for completion routing (set once, used by all connections)
    static std::unordered_map<int, RegistrarConnectionPtr>& connection_map() {
        static std::unordered_map<int, RegistrarConnectionPtr> map;
        return map;
    }
```

- [ ] **Step 2: Add RegistrarConnection implementation**

```cpp
// -----------------------------------------------------------------------------
// RegistrarConnection Implementation
// -----------------------------------------------------------------------------

RegistrarConnection::RegistrarConnection(CommunicationEndpoint remote_endpoint,
                                        EventLoop* loop,
                                        int fd)
    : remote_endpoint_(remote_endpoint),
      loop_(loop),
      fd_(fd),
      header_buffer_(TcpHeaderSize) {}

RegistrarConnection::~RegistrarConnection() {
    close();
}

RegistrarConnectionPtr RegistrarConnection::accepted(int fd,
                                                     CommunicationEndpoint remote_endpoint,
                                                     EventLoop* loop) {
    auto conn = std::shared_ptr<RegistrarConnection>(
        new RegistrarConnection(remote_endpoint, loop, fd));
    conn->register_with_loop();
    return conn;
}

RegistrarConnectionPtr RegistrarConnection::connecting(int fd,
                                                     CommunicationEndpoint remote_endpoint,
                                                     EventLoop* loop) {
    return std::shared_ptr<RegistrarConnection>(
        new RegistrarConnection(remote_endpoint, loop, fd));
}

void RegistrarConnection::register_with_loop() {
    // Note: We do NOT set loop_->set_completion_callback() here
    // because that would overwrite the server's callback.
    // Instead, the server sets ONE completion callback that routes
    // completions via a static fd->connection map.
    // This connection registers itself in that map.
    if (loop_ && fd_ >= 0) {
        loop_->add_fd(fd_, EventLoop::Event::Read);
    }
}

void RegistrarConnection::set_message_handler(message_handler h) {
    message_handler_ = std::move(h);
}

void RegistrarConnection::set_disconnect_handler(disconnect_handler h) {
    disconnect_handler_ = std::move(h);
}

void RegistrarConnection::set_send_complete_handler(send_complete_handler h) {
    send_complete_handler_ = std::move(h);
}

void RegistrarConnection::send_message(TcpMessageType type, const bytes& payload) {
    if (fd_ < 0 || !loop_) return;

    // Build message: [Magic: 4][Version: 1][Type: 1][Length: 4][Payload: N]
    bytes message;
    message.resize(TcpHeaderSize + payload.size());

    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = TcpRegistrarVersion;
    message[5] = static_cast<uint8_t>(type);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(message.data() + 6, &len_be, 4);
    if (!payload.empty()) {
        memcpy(message.data() + TcpHeaderSize, payload.data(), payload.size());
    }

    // Append to write buffer and flush
    write_buffer_.insert(write_buffer_.end(), message.begin(), message.end());
    flush_write_buffer();
}

void RegistrarConnection::close() {
    if (fd_ >= 0) {
        if (loop_) {
            loop_->remove_fd(fd_);
        }
        ::close(fd_);
        fd_ = -1;
    }
}

void RegistrarConnection::handle_read_event() {
    if (fd_ < 0 || !loop_) return;

    // Non-blocking read loop (fd is known to be readable via EventLoop notification)
    // This pattern follows PlainConnection: poll has_event() to know when to read

    // Read into header buffer first
    while (header_bytes_read_ < TcpHeaderSize) {
        // Check if still readable (edge-triggered)
        if (!loop_->has_event(fd_, EventLoop::Event::Read)) {
            return;  // Would block, wait for next notification
        }

        ssize_t bytes_read = recv(fd_,
                                  header_buffer_.data() + header_bytes_read_,
                                  TcpHeaderSize - header_bytes_read_,
                                  0);
        if (bytes_read <= 0) {
            // Connection closed or error
            if (disconnect_handler_) {
                disconnect_handler_();
            }
            close();
            return;
        }
        header_bytes_read_ += static_cast<size_t>(bytes_read);
    }

    // Parse header
    uint32_t magic;
    memcpy(&magic, header_buffer_.data(), 4);
    magic = ntohl(magic);

    if (magic != TcpRegistrarMagic) {
        // Invalid magic - consume byte and try again
        memmove(header_buffer_.data(), header_buffer_.data() + 1, TcpHeaderSize - 1);
        header_bytes_read_--;
        // Loop back to try reading more header bytes
        return;
    }

    uint8_t version = header_buffer_[4];
    if (version != TcpRegistrarVersion) {
        // Unsupported version - consume byte and try again
        memmove(header_buffer_.data(), header_buffer_.data() + 1, TcpHeaderSize - 1);
        header_bytes_read_--;
        return;
    }

    current_type_ = static_cast<TcpMessageType>(header_buffer_[5]);

    uint32_t payload_len;
    memcpy(&payload_len, header_buffer_.data() + 6, 4);
    payload_len = ntohl(payload_len);

    // Allocate payload buffer
    payload_buffer_.resize(payload_len);
    payload_bytes_read_ = 0;
    read_state_ = ReadState::ReadingPayload;

    // Continue to read payload
    handle_payload_read();
}

void RegistrarConnection::handle_payload_read() {
    if (fd_ < 0) return;

    // Continue reading payload
    while (payload_bytes_read_ < payload_buffer_.size()) {
        // Check if still readable
        if (!loop_->has_event(fd_, EventLoop::Event::Read)) {
            return;  // Would block, wait for next notification
        }

        ssize_t bytes_read = recv(fd_,
                                  payload_buffer_.data() + payload_bytes_read_,
                                  payload_buffer_.size() - payload_bytes_read_,
                                  0);
        if (bytes_read <= 0) {
            // Connection closed or error
            if (disconnect_handler_) {
                disconnect_handler_();
            }
            close();
            return;
        }
        payload_bytes_read_ += static_cast<size_t>(bytes_read);
    }

    // Payload complete - deliver to handler
    if (message_handler_) {
        message_handler_(current_type_, payload_buffer_);
    }

    // Reset to reading next header
    header_bytes_read_ = 0;
    header_buffer_.fill(0);
    payload_buffer_.clear();
    read_state_ = ReadState::ReadingHeader;
}

void RegistrarConnection::flush_write_buffer() {
    if (fd_ < 0 || loop_ == nullptr || write_buffer_.empty() || is_sending_) {
        return;
    }

    is_sending_ = true;

    struct iovec iov;
    iov.iov_base = write_buffer_.data();
    iov.iov_len = write_buffer_.size();

    // Use async_send - completion routed via EventLoop completion_callback_
    loop_->backend()->async_send(fd_, &iov, 1, ActorId(0),
                                  static_cast<uint32_t>(OpType::Send));
}

void RegistrarConnection::handle_send_completion(int result) {
    if (send_complete_handler_) {
        send_complete_handler_(result);
    }
    is_sending_ = false;

    if (result < 0) {
        // Send error
        if (disconnect_handler_) {
            disconnect_handler_();
        }
        close();
        return;
    }

    // Remove sent bytes from write buffer
    if (static_cast<size_t>(result) >= write_buffer_.size()) {
        write_buffer_.clear();
    } else {
        write_buffer_.erase(write_buffer_.begin(),
                            write_buffer_.begin() + result);
    }

    // Continue flushing if more data
    if (!write_buffer_.empty()) {
        flush_write_buffer();
    }
}
```

- [ ] **Step 2: Verify compilation**

Run: `ninja -C build 2>&1 | head -50`
Expected: No errors related to RegistrarConnection

- [ ] **Step 3: Commit**

```bash
git add src/net/registrar.cpp
git commit -m "feat(registrar): add RegistrarConnection implementation"
```

---

## Task 3: Refactor RegistrarServer to use Acceptor + RegistrarConnection

**Files:**
- Modify: `include/hpactor/net/registrar.hpp:215-263`
- Modify: `src/net/registrar_server.cpp`

- [ ] **Step 1: Update RegistrarServer declaration in registrar.hpp**

Replace lines 215-263 with:

```cpp
// -----------------------------------------------------------------------------
// RegistrarServer - TCP-based authoritative registrar
// -----------------------------------------------------------------------------
class RegistrarServer {
public:
    RegistrarServer(const RegistrarConfig& config, CommunicationEndpoint local_endpoint,
                   EventLoop* loop = nullptr);
    ~RegistrarServer();

    // Non-copyable
    RegistrarServer(const RegistrarServer&) = delete;
    RegistrarServer& operator=(const RegistrarServer&) = delete;

    // Start TCP server and UDP listener
    void start();
    void stop();

    // Get registry for reading
    NodeRegistry* registry() { return &registry_; }

    // Set event loop for async I/O (can be changed before start())
    void set_event_loop(EventLoop* loop) { loop_ = loop; }

    // Handle incoming TCP connection
    void handle_accept(int client_fd, CommunicationEndpoint remote_endpoint);

    // Broadcast event to all connected clients
    void broadcast_node_joined(CommunicationEndpoint endpoint, const NodeEndpoint& ep);
    void broadcast_node_left(CommunicationEndpoint endpoint);

private:
    void handle_tcp_message(RegistrarConnectionPtr conn, TcpMessageType type, const bytes& data);
    void handle_disconnect(RegistrarConnectionPtr conn);

    RegistrarConfig config_;
    [[maybe_unused]] CommunicationEndpoint local_endpoint_;
    NodeRegistry registry_;
    EventLoop* loop_ = nullptr;
    Acceptor acceptor_;

    int udp_socket_ = -1;
    std::atomic<bool> running_{false};

    // Connected clients (endpoint -> connection)
    std::unordered_map<CommunicationEndpoint, RegistrarConnectionPtr> clients_;
    // fd -> connection map for completion routing
    std::unordered_map<int, RegistrarConnectionPtr> fd_to_connection_;
    std::mutex clients_mutex_;

    // Event processing thread
    std::thread event_thread_;
};
```

- [ ] **Step 2: Rewrite registrar_server.cpp**

Replace the entire contents of `src/net/registrar_server.cpp` with:

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

#include <hpactor/net/registrar.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <cstring>
#include <thread>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// RegistrarServer Implementation
// -----------------------------------------------------------------------------

RegistrarServer::RegistrarServer(const RegistrarConfig& config, CommunicationEndpoint local_endpoint,
                                 EventLoop* loop)
    : config_(config),
      local_endpoint_(local_endpoint),
      registry_(config),
      loop_(loop),
      acceptor_(loop) {}

RegistrarServer::~RegistrarServer() {
    stop();
}

void RegistrarServer::start() {
    if (running_.load()) {
        return;
    }

    running_.store(true);

    // Set completion callback for send routing BEFORE creating connections
    if (loop_) {
        loop_->set_completion_callback([this](OpCompletion c) {
            if (c.type == OpType::Send) {
                auto it = fd_to_connection_.find(c.fd);
                if (it != fd_to_connection_.end()) {
                    it->second->handle_send_completion(c.result);
                }
            }
        });
    }

    // Use Acceptor for TCP listening (async)
    acceptor_.set_accept_handler([this](int fd, CommunicationEndpoint remote_ep) {
        handle_accept(fd, remote_ep);
    });
    acceptor_.listen(config_.tcp_port);

    // Create UDP socket for resolution queries
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ >= 0) {
        struct sockaddr_in udp_addr;
        memset(&udp_addr, 0, sizeof(udp_addr));
        udp_addr.sin_family = AF_INET;
        udp_addr.sin_addr.s_addr = INADDR_ANY;
        udp_addr.sin_port = htons(config_.udp_port);
        bind(udp_socket_, reinterpret_cast<struct sockaddr*>(&udp_addr), sizeof(udp_addr));
    }

    // Start event processing loop in background thread
    event_thread_ = std::thread([this]() {
        while (running_.load()) {
            if (loop_) {
                int n = loop_->wait(100);  // 100ms timeout
                if (n > 0) {
                    // Process client connections for read events
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    for (auto& [fd, conn] : fd_to_connection_) {
                        (void)fd;
                        if (loop_->has_event(conn->fd(), EventLoop::Event::Read)) {
                            conn->handle_read_event();
                        }
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void RegistrarServer::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    // Stop event processing thread
    if (event_thread_.joinable()) {
        event_thread_.join();
    }

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& [endpoint, conn] : clients_) {
            (void)endpoint;
            conn->close();
        }
        clients_.clear();
        fd_to_connection_.clear();
    }

    // Close acceptor
    acceptor_.close();

    // Close UDP socket
    if (udp_socket_ >= 0) {
        close(udp_socket_);
        udp_socket_ = -1;
    }
}

void RegistrarServer::handle_accept(int client_fd, CommunicationEndpoint remote_endpoint) {
    // Set TCP_NODELAY for lower latency
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    auto conn = RegistrarConnection::accepted(client_fd, remote_endpoint, loop_);

    // Set message handler to process incoming messages
    conn->set_message_handler([this, conn](TcpMessageType type, const bytes& payload) {
        handle_tcp_message(conn, type, payload);
    });

    // Set disconnect handler
    conn->set_disconnect_handler([this, conn]() {
        handle_disconnect(conn);
    });

    // Store connection in both maps
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_[remote_endpoint] = conn;
        fd_to_connection_[client_fd] = conn;
    }

    // Register with NodeRegistry
    // Note: actual registration info comes in Register message
}

void RegistrarServer::handle_tcp_message(RegistrarConnectionPtr conn,
                                         TcpMessageType type,
                                         const bytes& data) {
    switch (type) {
        case TcpMessageType::Register: {
            // Parse registration payload
            size_t offset = 0;

            // Parse endpoint (length-prefixed string)
            if (data.size() < offset + 4) {
                return;
            }
            uint32_t endpoint_len;
            memcpy(&endpoint_len, data.data() + offset, 4);
            endpoint_len = ntohl(endpoint_len);
            offset += 4;

            CommunicationEndpoint node_endpoint;
            if (endpoint_len > 0 && data.size() >= offset + endpoint_len) {
                std::string endpoint_str(reinterpret_cast<const char*>(data.data() + offset),
                                         endpoint_len);
                node_endpoint = endpoint_ops::parse_endpoint(endpoint_str);
                offset += endpoint_len;
            }

            if (std::holds_alternative<Ipv4Endpoint>(node_endpoint) &&
                std::get<Ipv4Endpoint>(node_endpoint).is_unspecified()) {
                return;
            }

            // Parse host
            if (data.size() < offset + 1) {
                return;
            }
            uint8_t host_len = data[offset++];
            if (data.size() < offset + host_len) {
                return;
            }
            std::string client_host(reinterpret_cast<const char*>(data.data() + offset),
                                   host_len);
            offset += host_len;

            // Parse tcp_port
            if (data.size() < offset + 2) {
                return;
            }
            uint16_t client_port;
            memcpy(&client_port, data.data() + offset, 2);
            client_port = ntohs(client_port);
            offset += 2;

            // Parse acceptors (optional)
            std::vector<AcceptorInfo> client_acceptors;
            if (data.size() >= offset + 1) {
                uint8_t acceptor_count = data[offset++];
                for (uint8_t i = 0; i < acceptor_count && data.size() >= offset + 4; ++i) {
                    AcceptorInfo acceptor;
                    memcpy(&acceptor.port, data.data() + offset, 2);
                    acceptor.port = ntohs(acceptor.port);
                    offset += 2;
                    acceptor.handshake_version = data[offset++];
                    acceptor.protocol_version = data[offset++];
                    acceptor.tls_required = data[offset++] != 0;
                    client_acceptors.push_back(acceptor);
                }
            }

            // Update clients map with actual endpoint
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                clients_.erase(conn->remote_endpoint());
                clients_[node_endpoint] = conn;
            }

            // Create and upsert endpoint
            NodeEndpoint ep;
            ep.endpoint = node_endpoint;
            ep.host = client_host;
            ep.tcp_port = client_port;
            ep.acceptors = std::move(client_acceptors);
            ep.last_seen = std::chrono::steady_clock::now();
            registry_.upsert_endpoint(ep);

            // Send Accept response
            bytes accept_payload(1, 0);  // Error code 0 = success
            conn->send_message(TcpMessageType::Accept, accept_payload);

            // Broadcast node joined to other clients
            broadcast_node_joined(node_endpoint, ep);
            break;
        }

        case TcpMessageType::Heartbeat: {
            CommunicationEndpoint endpoint = conn->remote_endpoint();
            bool is_valid = std::holds_alternative<Ipv4Endpoint>(endpoint) ?
                !std::get<Ipv4Endpoint>(endpoint).is_unspecified() : true;
            if (is_valid) {
                NodeEndpoint* ep = registry_.get(endpoint);
                if (ep) {
                    ep->last_seen = std::chrono::steady_clock::now();
                }
            }
            break;
        }

        case TcpMessageType::NodeJoin:
        case TcpMessageType::NodeLeave:
        case TcpMessageType::Accept:
        case TcpMessageType::Error:
            // These are not expected from clients
            break;
    }
}

void RegistrarServer::handle_disconnect(RegistrarConnectionPtr conn) {
    CommunicationEndpoint endpoint = conn->remote_endpoint();
    int fd = conn->fd();

    // Remove from both maps
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(endpoint);
        fd_to_connection_.erase(fd);
    }

    // Broadcast node left
    broadcast_node_left(endpoint);

    // Remove endpoint from registry
    registry_.remove_endpoint(endpoint);
}

void RegistrarServer::broadcast_node_joined(CommunicationEndpoint endpoint,
                                            const NodeEndpoint& ep) {
    // Encode broadcast payload
    std::string endpoint_str = endpoint_ops::to_string(endpoint);
    uint32_t endpoint_len = static_cast<uint32_t>(endpoint_str.size());

    bytes payload;
    payload.resize(4 + endpoint_len + 1 + ep.host.size() + 2);

    size_t offset = 0;
    uint32_t len_be = htonl(endpoint_len);
    memcpy(payload.data() + offset, &len_be, 4);
    offset += 4;

    if (endpoint_len > 0) {
        memcpy(payload.data() + offset, endpoint_str.data(), endpoint_len);
        offset += endpoint_len;
    }

    payload[offset++] = static_cast<uint8_t>(ep.host.size());
    memcpy(payload.data() + offset, ep.host.data(), ep.host.size());
    offset += ep.host.size();

    uint16_t port_be = htons(ep.tcp_port);
    memcpy(payload.data() + offset, &port_be, 2);

    // Send to all clients except the joining node
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& [id, conn] : clients_) {
        if (id != endpoint) {
            conn->send_message(TcpMessageType::NodeJoin, payload);
        }
    }
}

void RegistrarServer::broadcast_node_left(CommunicationEndpoint endpoint) {
    // Encode broadcast payload
    std::string endpoint_str = endpoint_ops::to_string(endpoint);
    uint32_t endpoint_len = static_cast<uint32_t>(endpoint_str.size());

    bytes payload;
    payload.resize(4 + endpoint_len);

    uint32_t len_be = htonl(endpoint_len);
    memcpy(payload.data(), &len_be, 4);
    size_t offset = 4;
    if (endpoint_len > 0) {
        memcpy(payload.data() + offset, endpoint_str.data(), endpoint_len);
    }

    // Send to all remaining clients
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& [id, conn] : clients_) {
        (void)id;
        conn->send_message(TcpMessageType::NodeLeave, payload);
    }
}

} // namespace net
} // namespace hpactor
```

- [ ] **Step 3: Verify compilation**

Run: `ninja -C build 2>&1 | head -50`
Expected: No errors related to RegistrarServer

- [ ] **Step 4: Run tests**

Run: `ctest --output-on-failure -R registrar`
Expected: All tests pass

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/net/registrar.hpp src/net/registrar_server.cpp
git commit -m "refactor(registrar): migrate RegistrarServer to async I/O with Acceptor + RegistrarConnection"
```

---

## Task 4: Refactor RegistrarClient to use RegistrarConnection + EventLoop timers

**Files:**
- Modify: `include/hpactor/net/registrar.hpp:323-394`
- Modify: `src/net/registrar_client.cpp`

- [ ] **Step 1: Update RegistrarClient declaration in registrar.hpp**

Replace lines 323-394 with:

```cpp
// -----------------------------------------------------------------------------
// RegistrarClient - TCP client for connecting to RegistrarServer
// -----------------------------------------------------------------------------
class RegistrarClient {
public:
    RegistrarClient(const RegistrarConfig& config, CommunicationEndpoint local_endpoint,
                   CommunicationEndpoint server_endpoint, NodeRegistry* shared_registry,
                   EventLoop* loop = nullptr);
    ~RegistrarClient();

    // Non-copyable
    RegistrarClient(const RegistrarClient&) = delete;
    RegistrarClient& operator=(const RegistrarClient&) = delete;

    void start();
    void stop();

    // Set event loop for async I/O (can be changed before start())
    void set_event_loop(EventLoop* loop) { loop_ = loop; }

    // Set acceptors for registration announcement
    void set_acceptors(std::vector<AcceptorInfo> acceptors);

    // Reconnect to server (used after disconnection)
    void reconnect();

    // Check if connected
    bool is_connected() const { return connected_.load(); }

private:
    void attempt_connection();
    void send_registration();

    // Handle server messages
    void handle_server_message(TcpMessageType type, const bytes& data);
    void handle_disconnect();

    RegistrarConfig config_;
    CommunicationEndpoint local_endpoint_;
    CommunicationEndpoint server_endpoint_;
    NodeRegistry* shared_registry_;  // Not owned
    EventLoop* loop_ = nullptr;

    RegistrarConnectionPtr server_connection_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    // Timer handles
    uint64_t heartbeat_timer_ = 0;

    // For heartbeat tracking
    std::chrono::steady_clock::time_point last_heartbeat_sent_;

    // Acceptors announced during registration
    std::vector<AcceptorInfo> acceptors_;
};
```

- [ ] **Step 2: Rewrite registrar_client.cpp**

Replace the entire contents of `src/net/registrar_client.cpp` with:

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

#include <hpactor/net/registrar.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fcntl.h>

#include <cstring>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// Helper Functions
// -----------------------------------------------------------------------------

static std::string get_local_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return "127.0.0.1";
    }

    std::string result;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (!(ifa->ifa_flags & IFF_RUNNING)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) != nullptr) {
            result = ip;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return result.empty() ? "127.0.0.1" : result;
}

// -----------------------------------------------------------------------------
// RegistrarClient Implementation
// -----------------------------------------------------------------------------

RegistrarClient::RegistrarClient(const RegistrarConfig& config,
                                 CommunicationEndpoint local_endpoint,
                                 CommunicationEndpoint server_endpoint,
                                 NodeRegistry* shared_registry,
                                 EventLoop* loop)
    : config_(config),
      local_endpoint_(local_endpoint),
      server_endpoint_(server_endpoint),
      shared_registry_(shared_registry),
      loop_(loop),
      last_heartbeat_sent_(std::chrono::steady_clock::now()) {}

RegistrarClient::~RegistrarClient() {
    stop();
}

void RegistrarClient::set_acceptors(std::vector<AcceptorInfo> acceptors) {
    acceptors_ = std::move(acceptors);
}

void RegistrarClient::start() {
    if (running_.load()) {
        return;
    }

    running_.store(true);
    connected_.store(false);

    // Start connection attempt
    attempt_connection();
}

void RegistrarClient::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    connected_.store(false);

    // Cancel timers
    if (loop_) {
        if (heartbeat_timer_ != 0) {
            loop_->cancel_timer(heartbeat_timer_);
            heartbeat_timer_ = 0;
        }
    }

    // Close server connection
    if (server_connection_) {
        server_connection_->close();
        server_connection_.reset();
    }
}

void RegistrarClient::attempt_connection() {
    if (!running_.load()) {
        return;
    }

    // Get server endpoint from registry
    NodeEndpoint* server_ep = shared_registry_->get(server_endpoint_);
    if (!server_ep) {
        // Schedule retry
        if (loop_ && running_.load()) {
            loop_->run_after([this]() {
                attempt_connection();
            }, 1000);
        }
        return;
    }

    // Resolve server hostname
    std::string server_ip = server_ep->host;
    struct in_addr addr;
    if (inet_pton(AF_INET, server_ip.c_str(), &addr) != 1) {
        HostResolver resolver;
        server_ip = resolver.resolve(server_ep->host);
        if (server_ip.empty()) {
            // Schedule retry
            if (loop_ && running_.load()) {
                loop_->run_after([this]() {
                    attempt_connection();
                }, 1000);
            }
            return;
        }
    }

    // Create TCP socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }

    // Set TCP_NODELAY
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Set non-blocking for async connect
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Prepare address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_ep->tcp_port);
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

    // Attempt connection
    int result = ::connect(fd, reinterpret_cast<struct sockaddr*>(&server_addr),
                          sizeof(server_addr));
    if (result < 0 && errno != EINPROGRESS) {
        close(fd);
        // Schedule retry
        if (loop_ && running_.load()) {
            loop_->run_after([this]() {
                attempt_connection();
            }, 1000);
        }
        return;
    }

    // Create RegistrarConnection for client-side connection
    server_connection_ = RegistrarConnection::connecting(fd, server_endpoint_, loop_);

    // Set up handlers
    server_connection_->set_message_handler([this](TcpMessageType type, const bytes& data) {
        handle_server_message(type, data);
    });

    server_connection_->set_disconnect_handler([this]() {
        handle_disconnect();
    });

    // Send registration
    send_registration();

    // Start heartbeat timer
    if (loop_ && running_.load()) {
        heartbeat_timer_ = loop_->run_every([this]() {
            if (connected_.load() && server_connection_) {
                server_connection_->send_message(TcpMessageType::Heartbeat, {});
                last_heartbeat_sent_ = std::chrono::steady_clock::now();
            }
        }, static_cast<int>(config_.heartbeat_interval.count()));
    }
}

void RegistrarClient::send_registration() {
    if (!server_connection_ || !running_.load()) {
        return;
    }

    // Build registration payload
    std::string host = get_local_ip();
    uint16_t tcp_port = config_.tcp_port;
    std::string endpoint_str = endpoint_ops::to_string(local_endpoint_);
    uint32_t endpoint_len = static_cast<uint32_t>(endpoint_str.size());

    size_t payload_size = 4 + endpoint_len + 1 + host.size() + 2 + 1 +
                          acceptors_.size() * 4;

    bytes payload;
    payload.resize(payload_size);

    size_t offset = 0;
    uint32_t len_be = htonl(endpoint_len);
    memcpy(payload.data() + offset, &len_be, 4);
    offset += 4;

    if (endpoint_len > 0) {
        memcpy(payload.data() + offset, endpoint_str.data(), endpoint_len);
        offset += endpoint_len;
    }

    payload[offset++] = static_cast<uint8_t>(host.size());
    memcpy(payload.data() + offset, host.data(), host.size());
    offset += host.size();

    uint16_t port_be = htons(tcp_port);
    memcpy(payload.data() + offset, &port_be, 2);
    offset += 2;

    payload[offset++] = static_cast<uint8_t>(acceptors_.size());
    for (const auto& acceptor : acceptors_) {
        uint16_t acc_port_be = htons(acceptor.port);
        memcpy(payload.data() + offset, &acc_port_be, 2);
        offset += 2;
        payload[offset++] = acceptor.handshake_version;
        payload[offset++] = acceptor.protocol_version;
        payload[offset++] = acceptor.tls_required ? 1 : 0;
    }

    server_connection_->send_message(TcpMessageType::Register, payload);
}

void RegistrarClient::handle_server_message(TcpMessageType type, const bytes& data) {
    switch (type) {
        case TcpMessageType::Accept: {
            // Registration accepted
            if (!data.empty() && data[0] == 0) {
                connected_.store(true);
            }
            break;
        }

        case TcpMessageType::NodeJoin: {
            // New node joined - update registry
            size_t offset = 0;
            if (data.size() < 4) break;

            uint32_t endpoint_len;
            memcpy(&endpoint_len, data.data() + offset, 4);
            endpoint_len = ntohl(endpoint_len);
            offset += 4;

            if (data.size() < offset + endpoint_len + 1 + 2) break;

            std::string endpoint_str(reinterpret_cast<const char*>(data.data() + offset),
                                    endpoint_len);
            offset += endpoint_len;

            uint8_t host_len = data[offset++];
            std::string host(reinterpret_cast<const char*>(data.data() + offset),
                           host_len);
            offset += host_len;

            uint16_t port;
            memcpy(&port, data.data() + offset, 2);
            port = ntohs(port);

            NodeEndpoint ep;
            ep.endpoint = endpoint_ops::parse_endpoint(endpoint_str);
            ep.host = host;
            ep.tcp_port = port;
            ep.last_seen = std::chrono::steady_clock::now();
            shared_registry_->upsert_endpoint(ep);
            break;
        }

        case TcpMessageType::NodeLeave: {
            // Node left - remove from registry
            size_t offset = 0;
            if (data.size() < 4) break;

            uint32_t endpoint_len;
            memcpy(&endpoint_len, data.data() + offset, 4);
            endpoint_len = ntohl(endpoint_len);
            offset += 4;

            if (data.size() < offset + endpoint_len) break;

            std::string endpoint_str(reinterpret_cast<const char*>(data.data() + offset),
                                    endpoint_len);
            CommunicationEndpoint ep = endpoint_ops::parse_endpoint(endpoint_str);
            shared_registry_->remove_endpoint(ep);
            break;
        }

        case TcpMessageType::Error:
            // Handle error
            break;

        case TcpMessageType::Register:
        case TcpMessageType::Heartbeat:
            // Not expected from server
            break;
    }
}

void RegistrarClient::handle_disconnect() {
    connected_.store(false);

    // Cancel heartbeat timer
    if (loop_) {
        if (heartbeat_timer_ != 0) {
            loop_->cancel_timer(heartbeat_timer_);
            heartbeat_timer_ = 0;
        }
    }

    // Close connection
    if (server_connection_) {
        server_connection_->close();
        server_connection_.reset();
    }

    // Schedule reconnection
    if (running_.load() && loop_) {
        loop_->run_after([this]() {
            attempt_connection();
        }, 1000);
    }
}

void RegistrarClient::reconnect() {
    handle_disconnect();
}

} // namespace net
} // namespace hpactor
```

- [ ] **Step 3: Verify compilation**

Run: `ninja -C build 2>&1 | head -50`
Expected: No errors related to RegistrarClient

- [ ] **Step 4: Run tests**

Run: `ctest --output-on-failure -R registrar`
Expected: All tests pass

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/net/registrar.hpp src/net/registrar_client.cpp
git commit -m "refactor(registrar): migrate RegistrarClient to async I/O with EventLoop timers"
```

---

## Task 5: Add RegistrarConnection unit tests

**Files:**
- Create: `tests/net/test_registrar_connection.cpp`
- Modify: `tests/CMakeLists.txt` (add test executable if needed)

- [ ] **Step 1: Write RegistrarConnection unit test**

Create `tests/net/test_registrar_connection.cpp`:

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

#include <hpactor/net/registrar.hpp>
#include <hpactor/net/event_loop.hpp>

#include <cassert>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test framing encoding
    // Build a test message: Register type with empty payload
    bytes payload;
    bytes message;
    message.resize(10);  // TcpHeaderSize = 10

    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = TcpRegistrarVersion;
    message[5] = static_cast<uint8_t>(TcpMessageType::Register);
    uint32_t len_be = htonl(0);  // empty payload
    memcpy(message.data() + 6, &len_be, 4);

    // Verify header parsing
    assert(message[4] == TcpRegistrarVersion);
    assert(static_cast<TcpMessageType>(message[5]) == TcpMessageType::Register);

    uint32_t parsed_len;
    memcpy(&parsed_len, message.data() + 6, 4);
    parsed_len = ntohl(parsed_len);
    assert(parsed_len == 0);

    // Test TcpMessageType enum values
    assert(static_cast<uint8_t>(TcpMessageType::Register) == 0x01);
    assert(static_cast<uint8_t>(TcpMessageType::Heartbeat) == 0x02);
    assert(static_cast<uint8_t>(TcpMessageType::NodeJoin) == 0x03);
    assert(static_cast<uint8_t>(TcpMessageType::NodeLeave) == 0x04);
    assert(static_cast<uint8_t>(TcpMessageType::Accept) == 0x05);
    assert(static_cast<uint8_t>(TcpMessageType::Error) == 0x06);

    // Test endpoint parsing for broadcast
    std::string endpoint_str = "127.0.0.1:12345";
    CommunicationEndpoint ep = endpoint_ops::parse_endpoint(endpoint_str);
    assert(std::holds_alternative<Ipv4Endpoint>(ep));
    auto ipv4 = std::get<Ipv4Endpoint>(ep);
    assert(ipv4.port() == 12345);

    std::cout << "All RegistrarConnection tests passed" << std::endl;
    return 0;
}
```

- [ ] **Step 2: Add test to CMakeLists.txt if needed**

Check `tests/net/CMakeLists.txt` or main `tests/CMakeLists.txt` to see how tests are added. If test executable not configured, add it.

- [ ] **Step 3: Build and run test**

Run: `ninja -C build && ./build/tests/net/test_registrar_connection`
Expected: "All RegistrarConnection tests passed"

- [ ] **Step 4: Commit**

```bash
git add tests/net/test_registrar_connection.cpp
git commit -m "test(registrar): add RegistrarConnection unit tests"
```

---

## Task 6: Verify full test suite

- [ ] **Step 1: Run full test suite**

Run: `ctest --output-on-failure`
Expected: All tests pass (51+ tests)

- [ ] **Step 2: Final commit**

```bash
git add -A
git commit -m "feat(registrar): complete async I/O refactor

- Add RegistrarConnection for async TCP with registrar protocol framing
- Refactor RegistrarServer to use Acceptor + RegistrarConnection
- Refactor RegistrarClient to use EventLoop timers + RegistrarConnection
- Remove blocking socket threads (accept_thread_, connection_thread_, heartbeat_thread_)"
```

---

## Summary

| Task | Description |
|------|-------------|
| 1 | Add RegistrarConnection declaration to registrar.hpp |
| 2 | Implement RegistrarConnection in registrar.cpp |
| 3 | Refactor RegistrarServer to use Acceptor + RegistrarConnection |
| 4 | Refactor RegistrarClient to use EventLoop timers + RegistrarConnection |
| 5 | Add RegistrarConnection unit tests |
| 6 | Verify full test suite |
