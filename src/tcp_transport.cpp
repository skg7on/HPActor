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

TcpTransport::TcpTransport(NodeId node_id)
    : node_id_(node_id), loop_(), acceptor_(&loop_) {}

TcpTransport::~TcpTransport() {
    stop_listening();
    // Close all connections
    for (auto& [node, conn] : connections_) {
        conn->close();
    }
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

    ConnectionPtr connection = std::make_shared<TcpConnection>(fd, remote_node);
    connections_[remote_node] = connection;

    return connection;
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
    auto it = connections_.find(remote_node);
    if (it != connections_.end()) {
        it->second->send(encoded);
    }
}

bool TcpTransport::is_connected(NodeId remote_node) const {
    auto it = connections_.find(remote_node);
    if (it != connections_.end()) {
        return it->second->state() == ConnectionState::Connected;
    }
    return false;
}

void TcpTransport::close_connection(NodeId remote_node) {
    auto it = connections_.find(remote_node);
    if (it != connections_.end()) {
        it->second->close();
        connections_.erase(it);
    }
}

void TcpTransport::handle_accept(int client_fd) {
    // For accepted connections, we don't know the remote node ID yet
    // The connection will be registered when we receive the handshake
    // TODO: implement proper node ID exchange during handshake
    ConnectionPtr conn = std::make_shared<TcpConnection>(client_fd, 0);
    (void)conn;
}

// -----------------------------------------------------------------------------
// TcpConnection implementation
// -----------------------------------------------------------------------------

TcpConnection::TcpConnection(int fd, NodeId remote_node)
    : Connection(remote_node), fd_(fd) {
    set_state(ConnectionState::Connected);
}

TcpConnection::~TcpConnection() {
    close();
}

void TcpConnection::send(const bytes& data) {
    if (fd_ < 0) {
        return;
    }

    // If no pending write, try direct send
    if (!write_pending_) {
        ssize_t result = ::send(fd_, data.data(), data.size(), 0);
        if (result > 0) {
            if (static_cast<size_t>(result) < data.size()) {
                // Partial send, buffer the rest
                write_buffer_.insert(write_buffer_.end(),
                                    data.begin() + result, data.end());
                write_pending_ = true;
            }
            return;
        }
        if (result < 0 && errno == EAGAIN) {
            // Would block, buffer it
            write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
            write_pending_ = true;
        }
    } else {
        // Append to existing buffer
        write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
    }
}

void TcpConnection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    set_state(ConnectionState::Disconnected);
}

void TcpConnection::set_fd(int fd) {
    fd_ = fd;
    set_state(ConnectionState::Connected);
}

void TcpConnection::handle_read() {
    if (fd_ < 0) {
        return;
    }

    bytes buffer(4096, 0);
    ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (n > 0) {
        buffer.resize(static_cast<size_t>(n));
        Connection::handle_read(buffer);
    } else if (n == 0) {
        // Connection closed
        close();
    }
}

void TcpConnection::handle_write() {
    if (fd_ < 0 || write_buffer_.empty()) {
        return;
    }

    ssize_t n = ::send(fd_, write_buffer_.data(), write_buffer_.size(), 0);
    if (n > 0) {
        write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + n);
        if (write_buffer_.empty()) {
            write_pending_ = false;
        }
    }
}

} // namespace net
} // namespace hpactor
