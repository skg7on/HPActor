#pragma once

#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/tls_context.hpp>
#include <hpactor/net/transport.hpp>

#include <unordered_map>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// TcpTransport - TCP implementation of Transport with TLS and pooling
// -----------------------------------------------------------------------------
class TcpTransport : public Transport {
public:
    TcpTransport(NodeId node_id,
                 const TlsConfig& tls_config,
                 const PoolConfig& pool_config,
                 NodeRegistry* registry = nullptr);
    ~TcpTransport() override;

    // Transport interface
    ConnectionPtr connect(NodeId remote_node,
                        const std::string& host,
                        uint16_t port) override;

    ConnectionPtr connect(NodeId remote_node) override;

    void listen(uint16_t port) override;
    void stop_listening() override;

    void send(const ActorAddress& target, const bytes& encoded) override;

    bool is_connected(NodeId remote_node) const override;
    NodeId node_id() const override { return node_id_; }

    void close_connection(NodeId remote_node) override;

private:
    void handle_accept(int client_fd);

    // Get or create a connection pool for a remote node
    std::shared_ptr<ConnectionPool> get_or_create_pool(NodeId remote_node);

    NodeId node_id_;
    EventLoop loop_;
    Acceptor acceptor_;
    TlsContext tls_context_;
    PoolConfig pool_config_;
    NodeRegistry* registry_ = nullptr;  // Optional registry for node lookup
    HostResolver host_resolver_;
    std::unordered_map<NodeId, std::shared_ptr<ConnectionPool>> pools_;
};

} // namespace net
} // namespace hpactor
