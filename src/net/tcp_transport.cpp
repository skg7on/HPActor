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

#include <hpactor/net/tcp_transport.hpp>

#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// TcpTransport implementation
// -----------------------------------------------------------------------------

TcpTransport::TcpTransport(CommunicationEndpoint endpoint,
                           const TlsConfig& tls_config,
                           const PoolConfig& pool_config, NodeRegistry* registry)
    : endpoint_(endpoint), loop_(), acceptor_(&loop_),
      tls_context_(TlsContext::from_config(tls_config)),
      pool_config_(pool_config), registry_(registry) {
    // Ensure UDS directory exists
    std::string uds_dir = "/tmp/hpactor";
    ::mkdir(uds_dir.c_str(), 0755); // Ignore error if exists

    // Set up completion callback to route send completions to TlsConnection
    completion_callback_ = [this](OpCompletion c) {
        if (c.type == OpType::Send) {
            auto it = connections_.find(c.fd);
            if (it != connections_.end()) {
                it->second->handle_send_completion(c.result);
            }
        }
    };
    loop_.set_completion_callback(completion_callback_);
}

TcpTransport::~TcpTransport() {
    stop_listening();
    // Abort all connection pools
    for (auto& [ep, pool] : pools_) {
        pool->abort();
    }
}

std::shared_ptr<ConnectionPool>
TcpTransport::get_or_create_pool(CommunicationEndpoint remote_endpoint) {
    auto it = pools_.find(remote_endpoint);
    if (it != pools_.end()) {
        return it->second;
    }
    auto pool = std::make_shared<ConnectionPool>(remote_endpoint, pool_config_,
                                                 &tls_context_, &loop_);
    pools_[remote_endpoint] = pool;
    // Set RPC handler if one has been registered
    if (rpc_handler_) {
        pool->set_rpc_handler(rpc_handler_);
    }
    // Set actor message handler if one has been registered
    if (actor_msg_handler_) {
        pool->set_actor_message_handler(actor_msg_handler_);
    }
    return pool;
}

ConnectionPtr TcpTransport::connect(CommunicationEndpoint remote_endpoint,
                                    const std::string& /*host*/, uint16_t port) {
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

    int result =
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return nullptr;
    }

    // Get or create the pool
    auto pool = get_or_create_pool(remote_endpoint);

    ConnectionPtr conn;
    if (pool_config_.use_tls) {
        // Create TLS connection
        auto tls_conn =
            TlsConnection::create_client(remote_endpoint, &tls_context_, &loop_);
        tls_conn->set_fd(fd);
        tls_conn->set_ready_handler(
            [pool](ConnectionPtr c) { pool->on_connection_ready(c); });
        tls_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
            pool->on_connection_error(c, e);
        });
        tls_conn->set_frame_handler(
            [pool](std::span<const uint8_t> data) { pool->on_frame_received(data); });
        conn = tls_conn;
        tls_conn->start_client_handshake();
    } else {
        // Create plain connection with connected fd
        auto plain_conn =
            PlainConnection::create_client(fd, remote_endpoint, &loop_);
        plain_conn->set_ready_handler(
            [pool](ConnectionPtr c) { pool->on_connection_ready(c); });
        plain_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
            pool->on_connection_error(c, e);
        });
        plain_conn->set_frame_handler(
            [pool](std::span<const uint8_t> data) { pool->on_frame_received(data); });
        conn = plain_conn;
    }

    // Add to pool and track by fd for completion routing
    pool->add_connection(conn);
    register_connection(conn, fd);

    return pool;
}

ConnectionPtr TcpTransport::connect(CommunicationEndpoint remote_endpoint) {
    if (!registry_) {
        return nullptr; // No registry configured
    }

    NodeEndpoint* ep = registry_->get(remote_endpoint);
    if (!ep) {
        return nullptr; // Unknown node
    }

    // Check if UDS path is available for this endpoint
    if (!ep->uds_path.empty()) {
        return connect_unix_domain(remote_endpoint, ep->uds_path);
    }

    // Resolve hostname to IP if needed
    std::string ip = host_resolver_.resolve(ep->host);

    // Connect to resolved IP:port
    return connect(remote_endpoint, ip, ep->tcp_port);
}

ConnectionPtr
TcpTransport::connect_unix_domain(CommunicationEndpoint remote_endpoint,
                                  const std::string& socket_path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return nullptr;
    }

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    int result =
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return nullptr;
    }

    // Note: TLS over UDS is not supported per design decision.
    // use_tls config only applies to TCP connections.
    // UDS connections always use PlainConnection.
    auto pool = get_or_create_pool(remote_endpoint);
    ConnectionPtr conn;
    auto plain_conn = PlainConnection::create_client(fd, remote_endpoint, &loop_);
    plain_conn->set_ready_handler(
        [pool](ConnectionPtr c) { pool->on_connection_ready(c); });
    plain_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
        pool->on_connection_error(c, e);
    });
    plain_conn->set_frame_handler(
        [pool](std::span<const uint8_t> data) { pool->on_frame_received(data); });
    conn = plain_conn;

    pool->add_connection(conn);
    register_connection(conn, fd);

    return conn;
}

void TcpTransport::listen(uint16_t port) {
    acceptor_.set_accept_handler([this](int client_fd, CommunicationEndpoint ep) {
        handle_accept(client_fd, ep);
    });
    acceptor_.listen(port);
}

void TcpTransport::stop_listening() {
    acceptor_.close();
}

void TcpTransport::send(const ActorAddress& target, const bytes& encoded) {
    auto pool = get_or_create_pool(target.endpoint);
    pool->send(target, encoded);
}

bool TcpTransport::is_connected(CommunicationEndpoint remote_endpoint) const {
    auto it = pools_.find(remote_endpoint);
    if (it != pools_.end()) {
        return it->second->is_connected();
    }
    return false;
}

void TcpTransport::close_connection(CommunicationEndpoint remote_endpoint) {
    auto it = pools_.find(remote_endpoint);
    if (it != pools_.end()) {
        it->second->abort();
        pools_.erase(it);
    }
}

void TcpTransport::set_rpc_handler(rpc_response_handler handler) {
    // Store handler and apply to all existing pools
    rpc_handler_ = std::move(handler);
    for (auto& [ep, pool] : pools_) {
        pool->set_rpc_handler(rpc_handler_);
    }
}

void TcpTransport::register_connection(ConnectionPtr conn, int fd) {
    connections_[fd] = conn;
}

void TcpTransport::unregister_connection(int fd) {
    connections_.erase(fd);
}

void TcpTransport::handle_accept(int client_fd,
                                 CommunicationEndpoint remote_endpoint) {
    // Get or create pool for the connecting endpoint
    auto pool = get_or_create_pool(remote_endpoint);

    ConnectionPtr conn;
    if (pool_config_.use_tls) {
        auto tls_conn = TlsConnection::create_server(client_fd, remote_endpoint,
                                                     &tls_context_, &loop_);
        tls_conn->set_frame_handler(
            [pool](std::span<const uint8_t> data) { pool->on_frame_received(data); });
        tls_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
            pool->on_connection_error(c, e);
        });
        conn = tls_conn;
    } else {
        auto plain_conn = PlainConnection::create_server(client_fd, remote_endpoint, &loop_);
        plain_conn->set_frame_handler(
            [pool](std::span<const uint8_t> data) { pool->on_frame_received(data); });
        plain_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
            pool->on_connection_error(c, e);
        });
        conn = plain_conn;
    }

    pool->add_connection(conn);
    register_connection(conn, client_fd);
}

std::string TcpTransport::derive_uds_path(const std::string& node_id) const {
    // Sanitize node_id: replace colons with underscores
    std::string sanitized = node_id;
    for (char& c : sanitized) {
        if (c == ':')
            c = '_';
    }
    return "/tmp/hpactor/" + sanitized + ".sock";
}

} // namespace net
} // namespace hpactor
