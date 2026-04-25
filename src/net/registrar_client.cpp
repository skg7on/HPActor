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
#include <hpactor/net/registrar_serialization.hpp>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// Helper Functions
// -----------------------------------------------------------------------------

static std::string get_local_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return "127.0.0.1"; // Fallback
    }

    // Prefer non-loopback, up, running interfaces
    std::string result;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;
        if (!(ifa->ifa_flags & IFF_RUNNING))
            continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;
        if (ifa->ifa_addr->sa_family != AF_INET)
            continue;

        struct sockaddr_in* addr =
            reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) != nullptr) {
            result = ip;
            break; // Take first valid non-loopback
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

RegistrarClient::RegistrarClient(const RegistrarConfig& config,
                                 CommunicationEndpoint local_endpoint,
                                 CommunicationEndpoint server_endpoint,
                                 NodeRegistry* shared_registry, EventLoop* loop)
    : config_(config), local_endpoint_(local_endpoint),
      server_endpoint_(server_endpoint), shared_registry_(shared_registry),
      loop_(loop), last_heartbeat_sent_(std::chrono::steady_clock::now()) {}

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
        heartbeat_timer_ = loop_->run_every(
            [this]() {
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

                    server_connection_->send_message(TcpMessageType::Heartbeat,
                                                     bytes{});
                }
            },
            static_cast<int>(config_.heartbeat_interval.count()));
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
            loop_->run_after(
                [this]() {
                    if (running_.load()) {
                        attempt_connection();
                    }
                },
                1000);
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
                loop_->run_after(
                    [this]() {
                        if (running_.load()) {
                            attempt_connection();
                        }
                    },
                    1000);
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

    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&server_addr),
                  sizeof(server_addr)) < 0) {
        close(sock);
        // Schedule retry
        if (loop_) {
            loop_->run_after(
                [this]() {
                    if (running_.load()) {
                        attempt_connection();
                    }
                },
                1000);
        }
        return;
    }

    // Create RegistrarConnection wrapper
    server_connection_ =
        RegistrarConnection::connecting(sock, server_endpoint_, loop_);

    // Set up message handler
    server_connection_->set_message_handler(
        [this](TcpMessageType type, const bytes& data) {
            handle_server_message(type, data);
        });

    // Set up disconnect handler
    server_connection_->set_disconnect_handler([this]() { handle_disconnect(); });

    // Connection successful - will send registration after receiving connection
    // ready
    connected_.store(true);

    // Send registration
    send_registration();
}

void RegistrarClient::send_registration() {
    if (!server_connection_ || !connected_.load()) {
        return;
    }

    // Build registration message using protobuf
    std::string host = get_local_ip();
    uint16_t tcp_port = config_.tcp_port;

    // Create NodeEndpoint for serialization
    NodeEndpoint ep;
    ep.endpoint = local_endpoint_;
    ep.host = host;
    ep.tcp_port = tcp_port;
    ep.acceptors = acceptors_;

    bytes payload = serialize_register_payload(ep);
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
            PbNodeJoinPayload msg;
            if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
                break;
            }

            const auto& ep_info = msg.endpoint_info();
            std::string endpoint_str = ep_info.endpoint();
            CommunicationEndpoint endpoint =
                endpoint_ops::parse_endpoint(endpoint_str);

            std::string host = ep_info.host();
            uint16_t tcp_port = static_cast<uint16_t>(ep_info.tcp_port());

            NodeEndpoint node_ep;
            node_ep.endpoint = endpoint;
            node_ep.host = host;
            node_ep.tcp_port = tcp_port;
            node_ep.last_seen = std::chrono::steady_clock::now();
            shared_registry_->upsert_endpoint(node_ep);
            break;
        }

        case TcpMessageType::NodeLeave: {
            PbNodeLeavePayload msg;
            if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
                break;
            }

            std::string endpoint_str = msg.endpoint();
            CommunicationEndpoint endpoint =
                endpoint_ops::parse_endpoint(endpoint_str);
            shared_registry_->remove_endpoint(endpoint);
            break;
        }

        case TcpMessageType::Error: {
            // Error response
            // Payload: [ErrorCode: 1][MessageLen: 4][Message: N]
            if (data.size() < 1)
                break;
            uint8_t error_code = data[0];
            (void)error_code; // Could log this
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
        loop_->run_after(
            [this]() {
                if (running_.load()) {
                    reconnect();
                }
            },
            1000);
    }
}

void RegistrarClient::reconnect() {
    handle_disconnect();
}

} // namespace net
} // namespace hpactor
