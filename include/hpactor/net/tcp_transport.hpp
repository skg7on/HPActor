#pragma once

#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/transport.hpp>

#include <unordered_map>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// TcpTransport - TCP implementation of Transport
// -----------------------------------------------------------------------------
class TcpTransport : public Transport {
public:
    TcpTransport(NodeId node_id);
    ~TcpTransport() override;

    // Transport interface
    ConnectionPtr connect(NodeId remote_node,
                        const std::string& host,
                        uint16_t port) override;

    void listen(uint16_t port) override;
    void stop_listening() override;

    void send(const ActorAddress& target, const bytes& encoded) override;

    bool is_connected(NodeId remote_node) const override;
    NodeId node_id() const override { return node_id_; }

    void close_connection(NodeId remote_node) override;

private:
    void handle_accept(int client_fd);

    NodeId node_id_;
    EventLoop loop_;
    Acceptor acceptor_;
    std::unordered_map<NodeId, ConnectionPtr> connections_;
};

// -----------------------------------------------------------------------------
// TcpConnection - TCP implementation of Connection
// -----------------------------------------------------------------------------
class TcpConnection : public Connection {
public:
    TcpConnection(int fd, NodeId remote_node);
    ~TcpConnection() override;

    // Connection interface
    void send(const bytes& data) override;
    void close() override;

    // Get underlying socket fd
    int fd() const { return fd_; }

    // Set file descriptor (for accepted connections)
    void set_fd(int fd);

private:
    void handle_read();
    void handle_write();

    int fd_ = -1;
    bytes write_buffer_;
    bool write_pending_ = false;
};

} // namespace net
} // namespace hpactor
