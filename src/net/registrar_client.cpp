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

    if (loop_) {
        // Schedule heartbeat using EventLoop
        heartbeat_timer_ = loop_->run_every([this]() {
            if (connected_.load() && server_connection_) {
                // Build heartbeat message (no payload, just header)
                bytes message;
                message.resize(TcpHeaderSize);

                uint32_t magic_be = htonl(TcpRegistrarMagic);
                memcpy(message.data(), &magic_be, 4);
                message[4] = TcpRegistrarVersion;
                message[5] = static_cast<uint8_t>(TcpMessageType::Heartbeat);
                uint32_t len_be = htonl(0);
                memcpy(message.data() + 6, &len_be, 4);

                server_connection_->send_message(TcpMessageType::Heartbeat, bytes{});
            }
        }, static_cast<int>(config_.heartbeat_interval.count()));
    }

    // Start connection attempts
    attempt_connection();
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
        // Schedule retry if we have an event loop
        if (loop_) {
            loop_->run_after([this]() {
                if (running_.load()) {
                    attempt_connection();
                }
            }, 1000);
        }
        return;
    }

    // Resolve server hostname
    std::string server_ip = server_ep->host;
    struct in_addr addr;
    if (inet_pton(AF_INET, server_ip.c_str(), &addr) != 1) {
        // Try to resolve hostname
        HostResolver resolver;
        server_ip = resolver.resolve(server_ep->host);
        if (server_ip.empty()) {
            // Schedule retry
            if (loop_) {
                loop_->run_after([this]() {
                    if (running_.load()) {
                        attempt_connection();
                    }
                }, 1000);
            }
            return;
        }
    }

    // Create TCP socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return;
    }

    // Set TCP_NODELAY for lower latency
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Connect to server
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_ep->tcp_port);

    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        close(sock);
        return;
    }

    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        close(sock);
        // Schedule retry
        if (loop_) {
            loop_->run_after([this]() {
                if (running_.load()) {
                    attempt_connection();
                }
            }, 1000);
        }
        return;
    }

    // Create RegistrarConnection wrapper
    server_connection_ = RegistrarConnection::connecting(sock, server_endpoint_, loop_);

    // Set up message handler
    server_connection_->set_message_handler([this](TcpMessageType type, const bytes& data) {
        handle_server_message(type, data);
    });

    // Set up disconnect handler
    server_connection_->set_disconnect_handler([this]() {
        handle_disconnect();
    });

    // Connection successful - will send registration after receiving connection ready
    connected_.store(true);

    // Send registration
    send_registration();
}

void RegistrarClient::send_registration() {
    if (!server_connection_ || !connected_.load()) {
        return;
    }

    // Build registration message
    // Payload format: [Endpoint: 4][HostLen: 1][Host: N][TcpPort: 2][AcceptorCount: 1][Acceptors: ...]
    std::string host = get_local_ip();
    uint16_t tcp_port = config_.tcp_port;

    // Calculate payload size
    std::string endpoint_str = endpoint_ops::to_string(local_endpoint_);
    uint32_t node_id_len = static_cast<uint32_t>(endpoint_str.size());
    // Each acceptor: Port(2) + HandshakeVer(1) + ProtocolVer(1) + TlsRequired(1) = 5 bytes
    size_t payload_size = 4 + node_id_len + 1 + host.size() + 2 + 1;  // EndpointLen + Endpoint + HostLen + Host + TcpPort + AcceptorCount
    payload_size += acceptors_.size() * 5;  // Each acceptor: 5 bytes

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

    server_connection_->send_message(TcpMessageType::Register, payload);
}

void RegistrarClient::handle_server_message(TcpMessageType type, const bytes& data) {
    switch (type) {
        case TcpMessageType::Accept: {
            // Registration accepted - server acknowledges our registration
            // Payload: [ErrorCode: 1]
            if (data.size() >= 1) {
                uint8_t error_code = data[0];
                if (error_code == 0) {
                    // Success - we're registered
                    last_heartbeat_sent_ = std::chrono::steady_clock::now();
                }
            }
            break;
        }

        case TcpMessageType::NodeJoin: {
            // Another node joined - update registry
            // Payload format: [EndpointLen: 4][Endpoint: N][HostLen: 1][Host: N][Port: 2][AcceptorCount: 1][Acceptors...]
            size_t offset = 0;

            if (data.size() < 5) break;  // Need at least endpoint len + host len + port

            uint32_t endpoint_len;
            memcpy(&endpoint_len, data.data() + offset, 4);
            endpoint_len = ntohl(endpoint_len);
            offset += 4;

            if (data.size() < offset + endpoint_len + 1 + 2) break;

            CommunicationEndpoint endpoint;
            if (endpoint_len > 0) {
                std::string endpoint_str(reinterpret_cast<const char*>(data.data() + offset), endpoint_len);
                endpoint = endpoint_ops::parse_endpoint(endpoint_str);
                offset += endpoint_len;
            }

            uint8_t host_len = data[offset++];

            if (data.size() < offset + host_len + 2) break;

            std::string host(reinterpret_cast<const char*>(data.data() + offset), host_len);
            offset += host_len;

            uint16_t tcp_port;
            memcpy(&tcp_port, data.data() + offset, 2);
            tcp_port = ntohs(tcp_port);
            offset += 2;

            // Parse acceptors
            if (data.size() < offset + 1) break;
            uint8_t acceptor_count = data[offset++];

            std::vector<AcceptorInfo> acceptors;
            for (uint8_t i = 0; i < acceptor_count && offset + 5 <= data.size(); ++i) {
                AcceptorInfo info;
                memcpy(&info.port, data.data() + offset, 2);
                info.port = ntohs(info.port);
                offset += 2;

                info.handshake_version = data[offset++];
                info.protocol_version = data[offset++];
                info.tls_required = data[offset++] != 0;
                acceptors.push_back(info);
            }

            // Add to shared registry
            NodeEndpoint node_ep;
            node_ep.endpoint = endpoint;
            node_ep.host = host;
            node_ep.tcp_port = tcp_port;
            node_ep.acceptors = std::move(acceptors);
            node_ep.last_seen = std::chrono::steady_clock::now();
            shared_registry_->upsert_endpoint(node_ep);
            break;
        }

        case TcpMessageType::NodeLeave: {
            // Node left - remove from registry
            // Payload format: [EndpointLen: 4][Endpoint: N]
            size_t offset = 0;

            if (data.size() < 4) break;

            uint32_t endpoint_len;
            memcpy(&endpoint_len, data.data() + offset, 4);
            endpoint_len = ntohl(endpoint_len);
            offset += 4;

            if (data.size() < offset + endpoint_len) break;

            if (endpoint_len > 0) {
                std::string endpoint_str(reinterpret_cast<const char*>(data.data() + offset), endpoint_len);
                CommunicationEndpoint endpoint = endpoint_ops::parse_endpoint(endpoint_str);
                shared_registry_->remove_endpoint(endpoint);
            }
            break;
        }

        case TcpMessageType::Error: {
            // Error response
            // Payload: [ErrorCode: 1][MessageLen: 4][Message: N]
            if (data.size() < 1) break;
            uint8_t error_code = data[0];
            (void)error_code;  // Could log this
            break;
        }

        case TcpMessageType::Heartbeat:
        case TcpMessageType::Register:
            // These are sent by us, not received
            break;
    }
}

void RegistrarClient::handle_disconnect() {
    connected_.store(false);

    // Close existing connection
    if (server_connection_) {
        server_connection_->close();
        server_connection_.reset();
    }

    // Schedule reconnect if still running
    if (running_.load() && loop_) {
        loop_->run_after([this]() {
            if (running_.load()) {
                reconnect();
            }
        }, 1000);
    }
}

void RegistrarClient::reconnect() {
    handle_disconnect();
}

} // namespace net
} // namespace hpactor
