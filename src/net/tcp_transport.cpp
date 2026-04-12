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
                           const PoolConfig& pool_config)
    : node_id_(node_id),
      loop_(),
      acceptor_(&loop_),
      tls_context_(TlsContext::from_config(tls_config)),
      pool_config_(pool_config) {}

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
