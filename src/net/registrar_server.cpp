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

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// RegistrarServer Implementation
// -----------------------------------------------------------------------------

RegistrarServer::RegistrarServer(const RegistrarConfig& config, NodeId local_node_id, EventLoop* loop)
    : config_(config),
      local_node_id_(local_node_id),
      registry_(config),
      loop_(loop) {}

RegistrarServer::~RegistrarServer() {
    stop();
}

void RegistrarServer::start() {
    if (running_.load()) {
        return;
    }

    // Create TCP socket
    tcp_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket_ < 0) {
        return;
    }

    // Allow address reuse
    int reuse = 1;
    setsockopt(tcp_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind TCP to tcp_port
    struct sockaddr_in tcp_addr;
    memset(&tcp_addr, 0, sizeof(tcp_addr));
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_addr.s_addr = INADDR_ANY;
    tcp_addr.sin_port = htons(config_.tcp_port);

    if (bind(tcp_socket_, reinterpret_cast<struct sockaddr*>(&tcp_addr), sizeof(tcp_addr)) < 0) {
        close(tcp_socket_);
        tcp_socket_ = -1;
        return;
    }

    listen(tcp_socket_, 5);

    // Create UDP socket for resolution - use udp_port
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ >= 0) {
        struct sockaddr_in udp_addr;
        memset(&udp_addr, 0, sizeof(udp_addr));
        udp_addr.sin_family = AF_INET;
        udp_addr.sin_addr.s_addr = INADDR_ANY;
        udp_addr.sin_port = htons(config_.udp_port);
        bind(udp_socket_, reinterpret_cast<struct sockaddr*>(&udp_addr), sizeof(udp_addr));
    }

    running_.store(true);

    // Use EventLoop's async_accept if available
    if (loop_) {
        // Register fd with event loop for read events
        loop_->add_fd(tcp_socket_, EventLoop::Event::Read);
        // Submit async_accept - completions delivered via EventLoop
        // Use ActorId(0) as sentinel; RegistrarServer handles accept via loop
    } else {
        // Fall back to blocking accept thread
        accept_thread_ = std::thread(&RegistrarServer::accept_loop, this);
    }
}

void RegistrarServer::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& [node_id, fd] : clients_) {
            (void)node_id;
            if (fd >= 0) {
                close(fd);
            }
        }
        clients_.clear();
    }

    // Remove from event loop if registered
    if (loop_ && tcp_socket_ >= 0) {
        loop_->remove_fd(tcp_socket_);
    }

    // Close accept thread's socket (accept_thread will close its copy)
    if (tcp_socket_ >= 0) {
        close(tcp_socket_);
        tcp_socket_ = -1;
    }

    if (udp_socket_ >= 0) {
        close(udp_socket_);
        udp_socket_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void RegistrarServer::send_tcp_response(int client_fd, TcpMessageType type, const bytes& payload) {
    bytes message;
    message.resize(TcpHeaderSize + payload.size());

    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = TcpRegistrarVersion;
    message[5] = static_cast<uint8_t>(type);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(message.data() + 6, &len_be, 4);
    memcpy(message.data() + TcpHeaderSize, payload.data(), payload.size());

    send(client_fd, message.data(), message.size(), 0);
}

void RegistrarServer::accept_loop() {
    while (running_.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(tcp_socket_, &read_fds);

        struct timeval tv = {1, 0}; // 1 second timeout
        int ret = select(static_cast<int>(tcp_socket_ + 1), &read_fds, nullptr, nullptr, &tv);

        if (ret > 0 && FD_ISSET(tcp_socket_, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(tcp_socket_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
            if (client_fd >= 0) {
                handle_accept(client_fd);
            }
        }
    }
}

void RegistrarServer::handle_accept(int client_fd) {
    // Set TCP_NODELAY to disable Nagle's algorithm for lower latency
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // First, read and process the Register message to get the NodeId
    // This is a blocking read with timeout
    NodeId node_id = 0;
    std::string client_host;
    uint16_t client_port = 0;
    std::vector<AcceptorInfo> client_acceptors;

    {
        // Set socket timeout for initial registration read
        struct timeval tv = {5, 0};  // 5 second timeout
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t header[TcpHeaderSize];
        ssize_t peeked = recv(client_fd, header, sizeof(header), MSG_PEEK);

        if (peeked < 0) {
            close(client_fd);
            return;
        }

        if (static_cast<size_t>(peeked) < TcpHeaderSize) {
            close(client_fd);
            return;
        }

        // Parse header
        uint32_t magic;
        memcpy(&magic, header, 4);
        magic = ntohl(magic);

        if (magic != TcpRegistrarMagic) {
            close(client_fd);
            return;
        }

        uint8_t version = header[4];
        if (version != TcpRegistrarVersion) {
            close(client_fd);
            return;
        }

        TcpMessageType type = static_cast<TcpMessageType>(header[5]);
        uint32_t length;
        memcpy(&length, header + 6, 4);
        length = ntohl(length);

        // Consume header
        recv(client_fd, header, TcpHeaderSize, 0);

        // Read payload
        bytes payload(length);
        size_t total_read = 0;
        while (total_read < length) {
            ssize_t bytes_read = recv(client_fd, payload.data() + total_read, length - total_read, 0);
            if (bytes_read <= 0) {
                break;
            }
            total_read += static_cast<size_t>(bytes_read);
        }

        if (total_read != length) {
            close(client_fd);
            return;
        }

        // Handle Register message to extract NodeId, Host, Port, Acceptors
        if (type == TcpMessageType::Register) {
            size_t offset = 0;

            // Parse NodeId (length-prefixed string)
            if (payload.size() < offset + 4) {
                close(client_fd);
                return;
            }
            uint32_t node_id_len;
            memcpy(&node_id_len, payload.data() + offset, 4);
            node_id_len = ntohl(node_id_len);
            offset += 4;

            if (payload.size() < offset + node_id_len) {
                close(client_fd);
                return;
            }
            if (node_id_len > 0) {
                node_id = std::string(reinterpret_cast<const char*>(payload.data() + offset), node_id_len);
                offset += node_id_len;
            }

            if (node_id.empty()) {
                close(client_fd);
                return;
            }

            // Parse Host
            if (payload.size() < offset + 1) {
                close(client_fd);
                return;
            }
            uint8_t host_len = payload[offset++];
            if (payload.size() < offset + host_len) {
                close(client_fd);
                return;
            }
            client_host = std::string(reinterpret_cast<const char*>(payload.data() + offset), host_len);
            offset += host_len;

            // Parse TcpPort
            if (payload.size() < offset + 2) {
                close(client_fd);
                return;
            }
            memcpy(&client_port, payload.data() + offset, 2);
            client_port = ntohs(client_port);
            offset += 2;

            // Parse Acceptors (optional, may not be present in older clients)
            if (payload.size() >= offset + 1) {
                uint8_t acceptor_count = payload[offset++];
                for (uint8_t i = 0; i < acceptor_count && payload.size() >= offset + 4; ++i) {
                    AcceptorInfo acceptor;
                    memcpy(&acceptor.port, payload.data() + offset, 2);
                    acceptor.port = ntohs(acceptor.port);
                    offset += 2;
                    acceptor.handshake_version = payload[offset++];
                    acceptor.protocol_version = payload[offset++];
                    acceptor.tls_required = payload[offset++] != 0;
                    client_acceptors.push_back(acceptor);
                }
            }
        } else {
            // First message must be Register
            close(client_fd);
            return;
        }
    }

    // Add client to clients map
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_[node_id] = client_fd;
    }

    // Create and upsert endpoint
    NodeEndpoint ep;
    ep.node_id = node_id;
    ep.host = client_host;
    ep.tcp_port = client_port;
    ep.acceptors = std::move(client_acceptors);
    ep.last_seen = std::chrono::steady_clock::now();
    registry_.upsert_endpoint(ep);

    // Send Accept response
    {
        bytes accept_payload(1, 0);  // Error code 0 = success
        send_tcp_response(client_fd, TcpMessageType::Accept, accept_payload);
    }

    // Broadcast node joined to other clients
    broadcast_node_joined(node_id, ep);

    // Now enter the normal message loop (remove timeout)
    {
        struct timeval tv = {0, 100000};  // 100ms timeout for normal operation
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    // Read and parse messages in a loop
    uint8_t header[TcpHeaderSize];

    while (running_.load()) {
        ssize_t peeked = recv(client_fd, header, sizeof(header), MSG_PEEK);
        if (peeked == 0) {
            // Connection closed
            break;
        }
        if (peeked < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }

        // Check if we have a full header
        if (static_cast<size_t>(peeked) < TcpHeaderSize) {
            // Need more data, skip peek and read to consume partial data
            ssize_t consumed = recv(client_fd, header, static_cast<size_t>(peeked), 0);
            (void)consumed;
            continue;
        }

        // Parse header: [Magic: 4][Version: 1][Type: 1][Length: 4]
        uint32_t magic;
        memcpy(&magic, header, 4);
        magic = ntohl(magic);

        if (magic != TcpRegistrarMagic) {
            // Invalid magic, consume and continue
            recv(client_fd, header, 1, 0);
            continue;
        }

        uint8_t version = header[4];
        if (version != TcpRegistrarVersion) {
            // Unsupported version, consume and continue
            recv(client_fd, header, 1, 0);
            continue;
        }

        TcpMessageType type = static_cast<TcpMessageType>(header[5]);

        uint32_t length;
        memcpy(&length, header + 6, 4);
        length = ntohl(length);

        // Consume the header
        ssize_t consumed = recv(client_fd, header, TcpHeaderSize, 0);
        (void)consumed;

        // Read payload
        bytes payload(length);
        size_t total_read = 0;
        while (total_read < length && running_.load()) {
            ssize_t bytes_read = recv(client_fd, payload.data() + total_read, length - total_read, 0);
            if (bytes_read <= 0) {
                break;
            }
            total_read += static_cast<size_t>(bytes_read);
        }

        if (total_read == length) {
            handle_tcp_message(client_fd, type, payload);
        }
    }

    // Remove from clients map and close
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto it = clients_.begin(); it != clients_.end(); ) {
            if (it->second == client_fd) {
                clients_.erase(it);
                break;
            }
            ++it;
        }
    }
    close(client_fd);

    // Broadcast node left
    broadcast_node_left(node_id);

    // Remove endpoint from registry
    registry_.remove_endpoint(node_id);
}

void RegistrarServer::handle_tcp_message(int client_fd, TcpMessageType type, const bytes& data) {
    switch (type) {
        case TcpMessageType::Register: {
            // Register is handled during accept - should not arrive here normally
            // But if client re-registers, we update their info
            size_t offset = 0;
            if (data.size() < 4) {
                return;
            }
            // Parse NodeId (length-prefixed string)
            uint32_t node_id_len;
            memcpy(&node_id_len, data.data() + offset, 4);
            node_id_len = ntohl(node_id_len);
            offset += 4;

            NodeId node_id;
            if (node_id_len > 0 && data.size() >= offset + node_id_len) {
                node_id = std::string(reinterpret_cast<const char*>(data.data() + offset), node_id_len);
                offset += node_id_len;
            }

            // Find this client's node_id in our map
            NodeId existing_node_id;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                for (const auto& [nid, fd] : clients_) {
                    if (fd == client_fd) {
                        existing_node_id = nid;
                        break;
                    }
                }
            }

            if (existing_node_id != node_id) {
                // NodeId mismatch - security concern, disconnect
                return;
            }

            // Update endpoint if host/port changed
            if (data.size() >= offset + 1) {
                uint8_t host_len = data[offset++];
                if (data.size() >= offset + host_len + 2) {
                    std::string host(reinterpret_cast<const char*>(data.data() + offset), host_len);
                    offset += host_len;

                    uint16_t port;
                    memcpy(&port, data.data() + offset, 2);
                    port = ntohs(port);

                    NodeEndpoint* ep = registry_.get(node_id);
                    if (ep) {
                        ep->host = host;
                        ep->tcp_port = port;
                        ep->last_seen = std::chrono::steady_clock::now();
                    }
                }
            }
            break;
        }

        case TcpMessageType::Heartbeat: {
            // Find client by fd and update last_seen
            NodeId node_id;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                for (const auto& [nid, fd] : clients_) {
                    if (fd == client_fd) {
                        node_id = nid;
                        break;
                    }
                }
            }

            if (!node_id.empty()) {
                NodeEndpoint* ep = registry_.get(node_id);
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
            // These are not expected from clients, ignore
            break;
    }
}

void RegistrarServer::broadcast_node_joined(NodeId node_id, const NodeEndpoint& ep) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    // Build NodeJoin message
    // Format: [Magic: 4][Version: 1][Type: 1][Length: 4][NodeIdLen: 4][NodeId: N][HostLen: 1][Host: N][Port: 2]
    bytes payload;
    uint32_t node_id_len = static_cast<uint32_t>(node_id.size());
    payload.resize(4 + node_id_len + 1 + ep.host.size() + 2);

    size_t offset = 0;
    uint32_t node_id_len_be = htonl(node_id_len);
    memcpy(payload.data() + offset, &node_id_len_be, 4);
    offset += 4;

    if (node_id_len > 0) {
        memcpy(payload.data() + offset, node_id.data(), node_id_len);
        offset += node_id_len;
    }

    payload[offset++] = static_cast<uint8_t>(ep.host.size());
    memcpy(payload.data() + offset, ep.host.data(), ep.host.size());
    offset += ep.host.size();

    uint16_t port_be = htons(ep.tcp_port);
    memcpy(payload.data() + offset, &port_be, 2);
    offset += 2;

    // Build full message with header
    bytes message;
    message.resize(TcpHeaderSize + payload.size());

    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = TcpRegistrarVersion;
    message[5] = static_cast<uint8_t>(TcpMessageType::NodeJoin);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(message.data() + 6, &len_be, 4);
    memcpy(message.data() + TcpHeaderSize, payload.data(), payload.size());

    // Send to all clients except the sender
    for (const auto& [id, fd] : clients_) {
        if (id != node_id && fd >= 0) {
            send(fd, message.data(), message.size(), 0);
        }
    }
}

void RegistrarServer::broadcast_node_left(NodeId node_id) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    // Build NodeLeave message
    // Format: [Magic: 4][Version: 1][Type: 1][Length: 4][NodeIdLen: 4][NodeId: N]
    uint32_t node_id_len = static_cast<uint32_t>(node_id.size());
    bytes payload;
    payload.resize(4 + node_id_len);

    uint32_t node_id_len_be = htonl(node_id_len);
    memcpy(payload.data(), &node_id_len_be, 4);
    size_t offset = 4;
    if (node_id_len > 0) {
        memcpy(payload.data() + offset, node_id.data(), node_id_len);
    }

    // Build full message with header
    bytes message;
    message.resize(TcpHeaderSize + payload.size());

    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = TcpRegistrarVersion;
    message[5] = static_cast<uint8_t>(TcpMessageType::NodeLeave);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(message.data() + 6, &len_be, 4);
    memcpy(message.data() + TcpHeaderSize, payload.data(), payload.size());

    // Send to all clients
    for (const auto& [id, fd] : clients_) {
        (void)id;
        if (fd >= 0) {
            send(fd, message.data(), message.size(), 0);
        }
    }
}

} // namespace net
} // namespace hpactor
