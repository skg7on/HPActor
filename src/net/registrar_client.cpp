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

#include <cstring>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// Helper Functions
// -----------------------------------------------------------------------------

static std::string get_local_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return "127.0.0.1";  // Fallback
    }

    // Prefer non-loopback, up, running interfaces
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
            break;  // Take first valid non-loopback
        }
    }

    freeifaddrs(ifaddr);
    return result.empty() ? "127.0.0.1" : result;
}

// -----------------------------------------------------------------------------
// RegistrarClient Implementation
// -----------------------------------------------------------------------------

void RegistrarClient::set_acceptors(std::vector<AcceptorInfo> acceptors) {
    acceptors_ = std::move(acceptors);
}

RegistrarClient::RegistrarClient(const RegistrarConfig& config, CommunicationEndpoint local_endpoint,
                                 CommunicationEndpoint server_endpoint, NodeRegistry* shared_registry,
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

void RegistrarClient::start() {
    if (running_.load()) {
        return;
    }

    running_.store(true);
    connected_.store(false);

    // Create UDP socket for resolution queries
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ < 0) {
        return;
    }

    if (loop_) {
        // Use EventLoop timers for heartbeat
        heartbeat_timer_ = loop_->run_every([this]() {
            if (connected_.load()) {
                send_heartbeat();
            }
        }, static_cast<int>(config_.heartbeat_interval.count()));
    }

    // Start connection thread
    connection_thread_ = std::thread(&RegistrarClient::connection_loop, this);
}

void RegistrarClient::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    connected_.store(false);

    // Cancel EventLoop timers
    if (loop_) {
        if (heartbeat_timer_ != 0) {
            loop_->cancel_timer(heartbeat_timer_);
            heartbeat_timer_ = 0;
        }
        if (connection_timer_ != 0) {
            loop_->cancel_timer(connection_timer_);
            connection_timer_ = 0;
        }
    }

    disconnect_from_server();

    if (udp_socket_ >= 0) {
        close(udp_socket_);
        udp_socket_ = -1;
    }

    if (connection_thread_.joinable()) {
        connection_thread_.join();
    }

    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

void RegistrarClient::connection_loop() {
    while (running_.load()) {
        if (!connected_.load()) {
            // Try to connect to server
            if (connect_to_server()) {
                // Send registration
                send_registration();
            } else {
                // Wait before retrying
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } else {
            // Monitor connection
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void RegistrarClient::heartbeat_loop() {
    while (running_.load()) {
        if (connected_.load()) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_heartbeat_sent_ >= config_.heartbeat_interval) {
                send_heartbeat();
                last_heartbeat_sent_ = now;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool RegistrarClient::connect_to_server() {
    // Get server endpoint from registry
    NodeEndpoint* server_ep = shared_registry_->get(server_endpoint_);
    if (!server_ep) {
        return false;
    }

    // Resolve server hostname
    std::string server_ip = server_ep->host;
    struct in_addr addr;
    if (inet_pton(AF_INET, server_ip.c_str(), &addr) != 1) {
        // Try to resolve hostname
        HostResolver resolver;
        server_ip = resolver.resolve(server_ep->host);
        if (server_ip.empty()) {
            return false;
        }
    }

    // Create TCP socket
    tcp_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket_ < 0) {
        return false;
    }

    // Set TCP_NODELAY for lower latency
    int nodelay = 1;
    setsockopt(tcp_socket_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Connect to server
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_ep->tcp_port);

    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        close(tcp_socket_);
        tcp_socket_ = -1;
        return false;
    }

    if (::connect(tcp_socket_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        close(tcp_socket_);
        tcp_socket_ = -1;
        return false;
    }

    connected_.store(true);
    return true;
}

void RegistrarClient::disconnect_from_server() {
    if (tcp_socket_ >= 0) {
        close(tcp_socket_);
        tcp_socket_ = -1;
    }
    connected_.store(false);
}

void RegistrarClient::send_registration() {
    if (tcp_socket_ < 0 || !connected_.load()) {
        return;
    }

    // Build registration message
    // Payload format: [Endpoint: 4][HostLen: 1][Host: N][TcpPort: 2][AcceptorCount: 1][Acceptors: ...]
    std::string host = get_local_ip();
    uint16_t tcp_port = config_.tcp_port;

    // Calculate payload size
    std::string endpoint_str = endpoint_ops::to_string(local_endpoint_);
    uint32_t node_id_len = static_cast<uint32_t>(endpoint_str.size());
    size_t payload_size = 4 + node_id_len + 1 + host.size() + 2 + 1;  // EndpointLen + Endpoint + HostLen + Host + TcpPort + AcceptorCount
    payload_size += acceptors_.size() * 4;  // Each acceptor: Port(2) + HandshakeVer(1) + ProtocolVer(1) + TlsRequired(1)

    bytes payload;
    payload.resize(payload_size);

    size_t offset = 0;
    uint32_t node_id_len_be = htonl(node_id_len);
    memcpy(payload.data() + offset, &node_id_len_be, 4);
    offset += 4;

    if (node_id_len > 0) {
        memcpy(payload.data() + offset, endpoint_str.data(), node_id_len);
        offset += node_id_len;
    }

    payload[offset++] = static_cast<uint8_t>(host.size());
    memcpy(payload.data() + offset, host.data(), host.size());
    offset += host.size();

    uint16_t port_be = htons(tcp_port);
    memcpy(payload.data() + offset, &port_be, 2);
    offset += 2;

    // Serialize acceptors
    payload[offset++] = static_cast<uint8_t>(acceptors_.size());
    for (const auto& acceptor : acceptors_) {
        uint16_t acc_port_be = htons(acceptor.port);
        memcpy(payload.data() + offset, &acc_port_be, 2);
        offset += 2;

        payload[offset++] = acceptor.handshake_version;
        payload[offset++] = acceptor.protocol_version;
        payload[offset++] = acceptor.tls_required ? 1 : 0;
    }

    // Build full message with TCP header
    bytes message;
    message.resize(TcpHeaderSize + payload.size());

    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = TcpRegistrarVersion;
    message[5] = static_cast<uint8_t>(TcpMessageType::Register);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(message.data() + 6, &len_be, 4);
    memcpy(message.data() + TcpHeaderSize, payload.data(), payload.size());

    send(tcp_socket_, message.data(), message.size(), 0);
}

void RegistrarClient::send_heartbeat() {
    if (tcp_socket_ < 0 || !connected_.load()) {
        return;
    }

    // Build heartbeat message (no payload, just header)
    bytes message;
    message.resize(TcpHeaderSize);

    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = TcpRegistrarVersion;
    message[5] = static_cast<uint8_t>(TcpMessageType::Heartbeat);
    uint32_t len_be = htonl(0);
    memcpy(message.data() + 6, &len_be, 4);

    send(tcp_socket_, message.data(), message.size(), 0);
}

NodeEndpoint* RegistrarClient::resolve_node(CommunicationEndpoint target_endpoint) {
    // First check local registry
    NodeEndpoint* ep = shared_registry_->get(target_endpoint);
    if (ep) {
        return ep;
    }

    // Need to query server via UDP
    if (udp_socket_ < 0) {
        return nullptr;
    }

    // Get server endpoint
    NodeEndpoint* server_ep = shared_registry_->get(server_endpoint_);
    if (!server_ep) {
        return nullptr;
    }

    // Build resolve query message
    // Payload format: [TargetEndpointLen: 4][TargetEndpoint: N]
    std::string target_str = endpoint_ops::to_string(target_endpoint);
    uint32_t node_id_len = static_cast<uint32_t>(target_str.size());
    bytes payload;
    payload.resize(4 + node_id_len);

    uint32_t node_id_len_be = htonl(node_id_len);
    memcpy(payload.data(), &node_id_len_be, 4);
    size_t offset = 4;
    if (node_id_len > 0) {
        memcpy(payload.data() + offset, target_str.data(), node_id_len);
    }

    // Build full message
    bytes message;
    message.resize(RegistrarHeaderSize + payload.size());

    uint32_t magic_be = htonl(RegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = RegistrarVersion;
    message[5] = static_cast<uint8_t>(RegistrarMessageType::ResolveQuery);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(message.data() + 6, &len_be, 4);
    memcpy(message.data() + RegistrarHeaderSize, payload.data(), payload.size());

    // Send to server's UDP port
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.udp_port);

    if (inet_pton(AF_INET, server_ep->host.c_str(), &server_addr.sin_addr) <= 0) {
        return nullptr;
    }

    ssize_t sent = sendto(udp_socket_, message.data(), message.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr));
    if (sent < 0) {
        return nullptr;
    }

    // Wait for response (with timeout)
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(static_cast<unsigned>(udp_socket_), &read_fds);

    struct timeval tv = {1, 0};  // 1 second timeout
    int ret = select(udp_socket_ + 1, &read_fds, nullptr, nullptr, &tv);

    if (ret > 0 && FD_ISSET(udp_socket_, &read_fds)) {
        bytes response(1024);
        struct sockaddr_in from_addr;
        socklen_t addr_len = sizeof(from_addr);

        ssize_t received = recvfrom(udp_socket_, response.data(), response.size(), 0,
                                     reinterpret_cast<struct sockaddr*>(&from_addr), &addr_len);

        if (received > 0) {
            // Parse response
            // Response format: [Magic: 4][Version: 1][Type: 1][Length: 4][NodeId: 4][HostLen: 1][Host: N][Port: 2]
            if (static_cast<size_t>(received) < RegistrarHeaderSize + 7) {
                return nullptr;
            }

            // Check magic and version
            uint32_t magic;
            memcpy(&magic, response.data(), 4);
            magic = ntohl(magic);
            if (magic != RegistrarMagic) {
                return nullptr;
            }

            uint8_t version = response[4];
            if (version != RegistrarVersion) {
                return nullptr;
            }

            RegistrarMessageType type = static_cast<RegistrarMessageType>(response[5]);
            if (type != RegistrarMessageType::ResolveResponse) {
                return nullptr;
            }

            // Parse endpoint info
            // Format: [EndpointLen: 4][Endpoint: N][HostLen: 1][Host: N][Port: 2]
            size_t resp_offset = RegistrarHeaderSize;

            uint32_t resp_node_id_len;
            memcpy(&resp_node_id_len, response.data() + resp_offset, 4);
            resp_node_id_len = ntohl(resp_node_id_len);
            resp_offset += 4;

            CommunicationEndpoint resp_endpoint;
            if (resp_node_id_len > 0 && response.size() >= resp_offset + resp_node_id_len) {
                std::string endpoint_str(reinterpret_cast<const char*>(response.data() + resp_offset), resp_node_id_len);
                resp_endpoint = endpoint_ops::parse_endpoint(endpoint_str);
                resp_offset += resp_node_id_len;
            }

            if (response.size() < resp_offset + 1) {
                return nullptr;
            }
            uint8_t host_len = response[resp_offset++];

            if (response.size() < resp_offset + host_len + 2) {
                return nullptr;
            }
            std::string resp_host(reinterpret_cast<const char*>(response.data() + resp_offset), host_len);
            resp_offset += host_len;

            uint16_t resp_port;
            memcpy(&resp_port, response.data() + resp_offset, 2);
            resp_port = ntohs(resp_port);

            // Add to shared registry
            NodeEndpoint new_ep;
            new_ep.endpoint = resp_endpoint;
            new_ep.host = resp_host;
            new_ep.tcp_port = resp_port;
            new_ep.last_seen = std::chrono::steady_clock::now();
            shared_registry_->upsert_endpoint(new_ep);

            return shared_registry_->get(resp_endpoint);
        }
    }

    return nullptr;
}

void RegistrarClient::reconnect() {
    // First, try to bind TCP port to become server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.tcp_port);

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        // Won the race - we could become server
        // For now, close and continue as client (full server mode in separate implementation)
        close(sock);

        // Notify that we need to run in server mode
        // In a full implementation, this would transition the node to server mode
        running_.store(false);
        connected_.store(false);
        return;
    }

    // Lost the bind race - another node became server
    close(sock);
    sock = -1;

    // Query UDP broadcast for any known nodes
    if (udp_socket_ < 0) {
        udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    }

    if (udp_socket_ >= 0) {
        // Broadcast probe on UDP
        struct sockaddr_in broadcast_addr;
        memset(&broadcast_addr, 0, sizeof(broadcast_addr));
        broadcast_addr.sin_family = AF_INET;
        broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
        broadcast_addr.sin_port = htons(config_.udp_port);

        // Enable broadcast
        int broadcast = 1;
        setsockopt(udp_socket_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        // Build probe message
        bytes payload;
        payload.resize(8);  // probe_id + timestamp
        // Fill with probe data...

        bytes message;
        message.resize(RegistrarHeaderSize + payload.size());

        uint32_t magic_be = htonl(RegistrarMagic);
        memcpy(message.data(), &magic_be, 4);
        message[4] = RegistrarVersion;
        message[5] = static_cast<uint8_t>(RegistrarMessageType::ResolveQuery);
        uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
        memcpy(message.data() + 6, &len_be, 4);
        memcpy(message.data() + RegistrarHeaderSize, payload.data(), payload.size());

        sendto(udp_socket_, message.data(), message.size(), 0,
               reinterpret_cast<struct sockaddr*>(&broadcast_addr), sizeof(broadcast_addr));

        // Wait for response
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(static_cast<unsigned>(udp_socket_), &read_fds);

        struct timeval tv = {2, 0};  // 2 second timeout
        int ret = select(udp_socket_ + 1, &read_fds, nullptr, nullptr, &tv);

        if (ret > 0 && FD_ISSET(udp_socket_, &read_fds)) {
            bytes response(1024);
            struct sockaddr_in from_addr;
            socklen_t addr_len = sizeof(from_addr);

            ssize_t received = recvfrom(udp_socket_, response.data(), response.size(), 0,
                                         reinterpret_cast<struct sockaddr*>(&from_addr), &addr_len);

            if (received > 0) {
                // Got a response - extract node info and update registry
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from_addr.sin_addr, ip_str, sizeof(ip_str));

                // Parse the response and update server_node_id_
                // Then reconnect...
            }
        }
    }

    // Reset connection state to trigger reconnection
    disconnect_from_server();
}

void RegistrarClient::failover() {
    reconnect();
}

void RegistrarClient::handle_connection_lost() {
    connected_.store(false);

    // Stop heartbeat thread
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    // Close TCP socket
    if (tcp_socket_ >= 0) {
        close(tcp_socket_);
        tcp_socket_ = -1;
    }

    // Try to become server (election)
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock >= 0) {
        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(config_.tcp_port);

        if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            // Won! We could become server
            // Close the socket - caller should transition to server mode
            close(sock);
            return;
        }
        close(sock);
    }

    // Can't become server - find one via broadcast
    find_server_via_broadcast();
}

void RegistrarClient::find_server_via_broadcast() {
    // Create UDP socket for broadcast
    int broadcast_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (broadcast_sock < 0) {
        return;
    }

    int broadcast_enable = 1;
    if (setsockopt(broadcast_sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        close(broadcast_sock);
        return;
    }

    struct sockaddr_in broadcast_addr;
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    broadcast_addr.sin_port = htons(config_.udp_port);

    // Build probe message
    // Format: [Magic: 4][Version: 1][Type: 1][Length: 4][ProbeId: 8][Timestamp: 8]
    bytes payload;
    payload.resize(16);  // 8 bytes probe_id + 8 bytes timestamp

    // Use hash of endpoint as probe_id and current time as timestamp
    uint64_t probe_id = std::hash<std::string>{}(endpoint_ops::to_string(local_endpoint_));
    uint64_t timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    size_t offset = 0;
    // Convert to big-endian manually
    uint64_t probe_id_be = ((probe_id & 0xFF00000000000000ULL) >> 56) |
                           ((probe_id & 0x00FF000000000000ULL) >> 40) |
                           ((probe_id & 0x0000FF0000000000ULL) >> 24) |
                           ((probe_id & 0x000000FF00000000ULL) >> 8) |
                           ((probe_id & 0x00000000FF000000ULL) << 8) |
                           ((probe_id & 0x0000000000FF0000ULL) << 24) |
                           ((probe_id & 0x000000000000FF00ULL) << 40) |
                           ((probe_id & 0x00000000000000FFULL) << 56);
    memcpy(payload.data() + offset, &probe_id_be, 8);
    offset += 8;

    uint64_t timestamp_be = ((timestamp & 0xFF00000000000000ULL) >> 56) |
                           ((timestamp & 0x00FF000000000000ULL) >> 40) |
                           ((timestamp & 0x0000FF0000000000ULL) >> 24) |
                           ((timestamp & 0x000000FF00000000ULL) >> 8) |
                           ((timestamp & 0x00000000FF000000ULL) << 8) |
                           ((timestamp & 0x0000000000FF0000ULL) << 24) |
                           ((timestamp & 0x000000000000FF00ULL) << 40) |
                           ((timestamp & 0x00000000000000FFULL) << 56);
    memcpy(payload.data() + offset, &timestamp_be, 8);
    offset += 8;

    bytes message;
    message.resize(RegistrarHeaderSize + payload.size());

    uint32_t magic_be = htonl(RegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = RegistrarVersion;
    message[5] = static_cast<uint8_t>(RegistrarMessageType::ResolveQuery);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(message.data() + 6, &len_be, 4);
    memcpy(message.data() + RegistrarHeaderSize, payload.data(), payload.size());

    // Send broadcast
    ssize_t sent = sendto(broadcast_sock, message.data(), message.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&broadcast_addr), sizeof(broadcast_addr));
    (void)sent;  // Ignore send errors, we'll timeout anyway

    // Wait for response
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(static_cast<unsigned>(broadcast_sock), &read_fds);

    struct timeval tv = {2, 0};  // 2 second timeout
    int ret = select(broadcast_sock + 1, &read_fds, nullptr, nullptr, &tv);

    if (ret > 0 && FD_ISSET(broadcast_sock, &read_fds)) {
        bytes response(1024);
        struct sockaddr_in from_addr;
        socklen_t addr_len = sizeof(from_addr);

        ssize_t received = recvfrom(broadcast_sock, response.data(), response.size(), 0,
                                     reinterpret_cast<struct sockaddr*>(&from_addr), &addr_len);

        if (received > 0) {
            // Parse response - looking for server's endpoint info
            // Response should contain server's node_id, host, port
            if (static_cast<size_t>(received) >= RegistrarHeaderSize + 7) {
                uint32_t resp_magic;
                memcpy(&resp_magic, response.data(), 4);
                resp_magic = ntohl(resp_magic);

                if (resp_magic == RegistrarMagic) {
                    uint8_t resp_version = response[4];
                    if (resp_version == RegistrarVersion) {
                        RegistrarMessageType resp_type = static_cast<RegistrarMessageType>(response[5]);

                        if (resp_type == RegistrarMessageType::ResolveResponse) {
                            // Parse the endpoint info
                            // Format: [EndpointLen: 4][Endpoint: N][HostLen: 1][Host: N][Port: 2]
                            size_t resp_offset = RegistrarHeaderSize;

                            uint32_t resp_node_id_len;
                            memcpy(&resp_node_id_len, response.data() + resp_offset, 4);
                            resp_node_id_len = ntohl(resp_node_id_len);
                            resp_offset += 4;

                            CommunicationEndpoint resp_endpoint;
                            if (resp_node_id_len > 0 && static_cast<size_t>(received) >= resp_offset + resp_node_id_len) {
                                std::string endpoint_str(reinterpret_cast<const char*>(response.data() + resp_offset), resp_node_id_len);
                                resp_endpoint = endpoint_ops::parse_endpoint(endpoint_str);
                                resp_offset += resp_node_id_len;
                            }

                            if (static_cast<size_t>(received) >= resp_offset + 1) {
                                uint8_t host_len = response[resp_offset++];
                                if (resp_offset + host_len + 2 <= static_cast<size_t>(received)) {
                                    std::string resp_host(reinterpret_cast<const char*>(response.data() + resp_offset), host_len);
                                    resp_offset += host_len;

                                    uint16_t resp_port;
                                    memcpy(&resp_port, response.data() + resp_offset, 2);
                                    resp_port = ntohs(resp_port);

                                    // Update registry with new server
                                    if (shared_registry_) {
                                        NodeEndpoint server_ep;
                                        server_ep.endpoint = resp_endpoint;
                                        server_ep.host = resp_host;
                                        server_ep.tcp_port = resp_port;
                                        server_ep.last_seen = std::chrono::steady_clock::now();
                                        shared_registry_->upsert_endpoint(server_ep);
                                    }

                                    // Update server_endpoint_ and reconnect
                                    server_endpoint_ = resp_endpoint;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    close(broadcast_sock);

    // Reset connection state to trigger reconnection
    disconnect_from_server();
}

} // namespace net
} // namespace hpactor