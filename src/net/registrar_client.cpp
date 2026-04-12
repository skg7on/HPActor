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
// RegistrarClient Implementation
// -----------------------------------------------------------------------------

RegistrarClient::RegistrarClient(const RegistrarConfig& config, NodeId local_node_id,
                                 NodeId server_node_id, NodeRegistry* shared_registry)
    : config_(config),
      local_node_id_(local_node_id),
      server_node_id_(server_node_id),
      shared_registry_(shared_registry),
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

    // Start connection thread
    connection_thread_ = std::thread(&RegistrarClient::connection_loop, this);

    // Start heartbeat thread
    heartbeat_thread_ = std::thread(&RegistrarClient::heartbeat_loop, this);
}

void RegistrarClient::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    connected_.store(false);

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
    NodeEndpoint* server_ep = shared_registry_->get(server_node_id_);
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
    // Payload format: [NodeId: 4][HostLen: 1][Host: N][TcpPort: 2]
    std::string host = "127.0.0.1";  // In production, get actual local IP
    uint16_t tcp_port = config_.tcp_port;

    bytes payload;
    payload.resize(4 + 1 + host.size() + 2);

    size_t offset = 0;
    uint32_t node_id_be = htonl(local_node_id_);
    memcpy(payload.data() + offset, &node_id_be, 4);
    offset += 4;

    payload[offset++] = static_cast<uint8_t>(host.size());
    memcpy(payload.data() + offset, host.data(), host.size());
    offset += host.size();

    uint16_t port_be = htons(tcp_port);
    memcpy(payload.data() + offset, &port_be, 2);
    offset += 2;

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

NodeEndpoint* RegistrarClient::resolve_node(NodeId node_id) {
    // First check local registry
    NodeEndpoint* ep = shared_registry_->get(node_id);
    if (ep) {
        return ep;
    }

    // Need to query server via UDP
    if (udp_socket_ < 0) {
        return nullptr;
    }

    // Get server endpoint
    NodeEndpoint* server_ep = shared_registry_->get(server_node_id_);
    if (!server_ep) {
        return nullptr;
    }

    // Build resolve query message
    // Payload format: [TargetNodeId: 4]
    bytes payload;
    payload.resize(4);

    uint32_t target_id_be = htonl(node_id);
    memcpy(payload.data(), &target_id_be, 4);

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
            size_t offset = RegistrarHeaderSize;

            uint32_t resp_node_id;
            memcpy(&resp_node_id, response.data() + offset, 4);
            resp_node_id = ntohl(resp_node_id);
            offset += 4;

            uint8_t host_len = response[offset++];

            std::string resp_host(reinterpret_cast<char*>(response.data() + offset), host_len);
            offset += host_len;

            uint16_t resp_port;
            memcpy(&resp_port, response.data() + offset, 2);
            resp_port = ntohs(resp_port);

            // Add to shared registry
            NodeEndpoint new_ep;
            new_ep.node_id = resp_node_id;
            new_ep.host = resp_host;
            new_ep.tcp_port = resp_port;
            new_ep.last_seen = std::chrono::steady_clock::now();
            shared_registry_->upsert_endpoint(new_ep);

            return shared_registry_->get(resp_node_id);
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

} // namespace net
} // namespace hpactor