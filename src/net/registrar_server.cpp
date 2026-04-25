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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <thread>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// RegistrarServer Implementation
// -----------------------------------------------------------------------------

RegistrarServer::RegistrarServer(const RegistrarConfig& config,
                                 CommunicationEndpoint local_endpoint,
                                 EventLoop* loop)
    : config_(config), local_endpoint_(local_endpoint), registry_(config),
      loop_(loop), acceptor_(loop) {}

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
        bind(udp_socket_, reinterpret_cast<struct sockaddr*>(&udp_addr),
             sizeof(udp_addr));
    }

    // Start event processing loop in background thread
    event_thread_ = std::thread([this]() {
        while (running_.load()) {
            if (loop_) {
                int n = loop_->wait(100); // 100ms timeout
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

void RegistrarServer::handle_accept(int client_fd,
                                    CommunicationEndpoint remote_endpoint) {
    // Set TCP_NODELAY for lower latency
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    auto conn = RegistrarConnection::accepted(client_fd, remote_endpoint, loop_);

    // Set message handler to process incoming messages
    conn->set_message_handler([this, conn](TcpMessageType type, const bytes& payload) {
        handle_tcp_message(conn, type, payload);
    });

    // Set disconnect handler
    conn->set_disconnect_handler([this, conn]() { handle_disconnect(conn); });

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
                                         TcpMessageType type, const bytes& data) {
    switch (type) {
        case TcpMessageType::Register: {
            PbRegisterPayload msg;
            if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
                return;
            }

            const auto& ep_info = msg.endpoint_info();
            std::string endpoint_str = ep_info.endpoint();
            CommunicationEndpoint node_endpoint =
                endpoint_ops::parse_endpoint(endpoint_str);

            if (std::holds_alternative<Ipv4Endpoint>(node_endpoint) &&
                std::get<Ipv4Endpoint>(node_endpoint).is_unspecified()) {
                return;
            }

            std::string client_host = ep_info.host();
            uint16_t client_port = static_cast<uint16_t>(ep_info.tcp_port());

            // Acceptors are at top level of PbRegisterPayload (per spec)
            std::vector<AcceptorInfo> client_acceptors;
            for (const auto& a : msg.acceptors()) {
                AcceptorInfo acceptor;
                acceptor.port = static_cast<uint16_t>(a.port());
                acceptor.handshake_version =
                    static_cast<uint8_t>(a.handshake_version());
                acceptor.protocol_version =
                    static_cast<uint8_t>(a.protocol_version());
                acceptor.tls_required = a.tls_required();
                client_acceptors.push_back(acceptor);
            }

            // Update clients map
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

            // Send Accept response using protobuf
            bytes accept_payload = serialize_accept_payload(0);
            conn->send_message(TcpMessageType::Accept, accept_payload);

            // Broadcast node joined
            broadcast_node_joined(node_endpoint, ep);
            break;
        }

        case TcpMessageType::Heartbeat: {
            CommunicationEndpoint endpoint = conn->remote_endpoint();
            bool is_valid = std::holds_alternative<Ipv4Endpoint>(endpoint)
                                ? !std::get<Ipv4Endpoint>(endpoint).is_unspecified()
                                : true;
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
    bytes payload = serialize_node_join_payload(ep);

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& [id, conn] : clients_) {
        if (id != endpoint) {
            conn->send_message(TcpMessageType::NodeJoin, payload);
        }
    }
}

void RegistrarServer::broadcast_node_left(CommunicationEndpoint endpoint) {
    bytes payload = serialize_node_leave_payload(endpoint);

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& [id, conn] : clients_) {
        (void)id;
        conn->send_message(TcpMessageType::NodeLeave, payload);
    }
}

} // namespace net
} // namespace hpactor
