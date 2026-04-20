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
#include <unistd.h>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// TcpTransport implementation
// -----------------------------------------------------------------------------

TcpTransport::TcpTransport(NodeId node_id,
                           const TlsConfig& tls_config,
                           const PoolConfig& pool_config,
                           NodeRegistry* registry)
    : node_id_(node_id),
      loop_(),
      acceptor_(&loop_),
      tls_context_(TlsContext::from_config(tls_config)),
      pool_config_(pool_config),
      registry_(registry) {}

TcpTransport::~TcpTransport() {
    stop_listening();
    // Abort all connection pools
    for (auto& [node, pool] : pools_) {
        pool->abort();
    }
}

std::shared_ptr<ConnectionPool> TcpTransport::get_or_create_pool(NodeId remote_node) {
    auto it = pools_.find(remote_node);
    if (it != pools_.end()) {
        return it->second;
    }
    auto pool = std::make_shared<ConnectionPool>(remote_node,
                                                  pool_config_,
                                                  &tls_context_,
                                                  &loop_);
    pools_[remote_node] = pool;
    // Set RPC handler if one has been registered
    if (rpc_handler_) {
        pool->set_rpc_handler(rpc_handler_);
    }
    return pool;
}

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

    // TODO: use inet_pton for actual address resolution
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int result = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return nullptr;
    }

    // Create a TlsConnection as client and get the pool
    auto pool = get_or_create_pool(remote_node);

    // Create client-side TLS connection
    auto conn = TlsConnection::create_client(remote_node, &tls_context_, &loop_);
    conn->start_client_handshake();

    // For the return value, we return the pool as a Connection
    // The pool itself manages the actual TlsConnection(s)
    return pool;
}

ConnectionPtr TcpTransport::connect(NodeId remote_node_id) {
    if (!registry_) {
        return nullptr;  // No registry configured
    }

    NodeEndpoint* ep = registry_->get(remote_node_id);
    if (!ep) {
        return nullptr;  // Unknown node
    }

    // Resolve hostname to IP if needed
    std::string ip = host_resolver_.resolve(ep->host);

    // Connect to resolved IP:port
    return connect(remote_node_id, ip, ep->tcp_port);
}

void TcpTransport::listen(uint16_t port) {
    acceptor_.set_accept_handler([this](int client_fd, NodeId /*remote_node_hint*/) {
        handle_accept(client_fd);
    });
    acceptor_.listen(port);
}

void TcpTransport::stop_listening() {
    acceptor_.close();
}

void TcpTransport::send(const ActorAddress& target, const bytes& encoded) {
    NodeId remote_node = target.node_id;
    auto pool = get_or_create_pool(remote_node);
    pool->send(target, encoded);
}

bool TcpTransport::is_connected(NodeId remote_node) const {
    auto it = pools_.find(remote_node);
    if (it != pools_.end()) {
        return it->second->is_connected();
    }
    return false;
}

void TcpTransport::close_connection(NodeId remote_node) {
    auto it = pools_.find(remote_node);
    if (it != pools_.end()) {
        it->second->abort();
        pools_.erase(it);
    }
}

void TcpTransport::set_rpc_handler(rpc_response_handler handler) {
    // Store handler and apply to all existing pools
    rpc_handler_ = std::move(handler);
    for (auto& [node, pool] : pools_) {
        pool->set_rpc_handler(rpc_handler_);
    }
}

void TcpTransport::handle_accept(int client_fd) {
    // For accepted connections, we don't know the remote node ID yet
    // The connection will be registered when we receive the handshake
    // TODO: implement proper node ID exchange during handshake
    // Create server-side TLS connection
    auto conn = TlsConnection::create_server(client_fd, 0, &tls_context_, &loop_);
    (void)conn;
}

} // namespace net
} // namespace hpactor
