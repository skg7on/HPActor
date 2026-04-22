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

#pragma once

#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/plain_connection.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/tls_connection.hpp>
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

    // Set RPC response handler - propagates to all connection pools
    void set_rpc_handler(rpc_response_handler handler) override;

private:
    void handle_accept(int client_fd);

    // Get or create a connection pool for a remote node
    std::shared_ptr<ConnectionPool> get_or_create_pool(NodeId remote_node);

    void register_connection(ConnectionPtr conn, int fd);
    void unregister_connection(int fd);

    NodeId node_id_;
    EventLoop loop_;
    Acceptor acceptor_;
    TlsContext tls_context_;
    PoolConfig pool_config_;
    NodeRegistry* registry_ = nullptr;  // Optional registry for node lookup
    HostResolver host_resolver_;
    std::unordered_map<NodeId, std::shared_ptr<ConnectionPool>> pools_;
    std::function<void(MessageId, const bytes&)> rpc_handler_;

    // Map of fd -> Connection for completion routing
    std::unordered_map<int, ConnectionPtr> connections_;

    // Completion callback for async send routing
    std::function<void(OpCompletion)> completion_callback_;
};

} // namespace net
} // namespace hpactor
