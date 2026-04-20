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

#include <hpactor/net/tls_context.hpp>
#include <hpactor/net/tls_connection.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// PoolConfig - connection pool configuration
// -----------------------------------------------------------------------------
struct PoolConfig {
    size_t min_connections = 1;
    size_t max_connections = 4;
    size_t max_pending = 1000;
    size_t max_attempts = 5;
    std::chrono::milliseconds initial_backoff{1000};
    std::chrono::milliseconds max_backoff{16000};
};

// Pending message entry
struct PendingMessage {
    ActorAddress target;
    bytes data;
    std::chrono::steady_clock::time_point enqueued_at;
};

// Connection pool statistics
struct PoolStats {
    size_t active_connections = 0;
    size_t pending_messages = 0;
    size_t reconnect_attempts = 0;
    bool is_connected = false;
};

class ConnectionPool : public Connection, public std::enable_shared_from_this<ConnectionPool> {
public:
    ConnectionPool(NodeId remote_node_id,
                   const PoolConfig& config,
                   TlsContext* tls_context,
                   EventLoop* loop);
    ~ConnectionPool();

    // Non-copyable
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // Connection interface - send raw bytes (uses default target)
    void send(const bytes& data) override;

    // Close the connection
    void close() override;

    // Send message to specific actor on remote node (uses pool)
    void send(const ActorAddress& target, const bytes& encoded);

    // Check if pool has active connections
    bool is_connected() const;

    // Get pool statistics
    PoolStats stats() const;

    // Graceful shutdown: drain pending messages
    // Returns number of messages that could not be sent
    size_t drain();

    // Immediate shutdown
    void abort();

    // Get remote node ID
    NodeId remote_node_id() const { return remote_node_id_; }

    // Set handler for RPC responses (called when RpcResponse frame is received)
    using rpc_response_handler = std::function<void(MessageId, const bytes&)>;
    void set_rpc_handler(rpc_response_handler handler);

private:
    // Get connection via round-robin
    TlsConnectionPtr get_connection();

    // Create new connection
    void create_connection();

    // Handle connection ready
    void on_connection_ready(TlsConnectionPtr conn);

    // Handle connection error
    void on_connection_error(TlsConnectionPtr conn, const error& err);

    // Handle incoming frame
    void on_frame_received(const bytes& frame_data);

    // Schedule reconnect with backoff
    void schedule_reconnect();

    // Flush pending messages
    void flush_pending();

    // Add pending message
    bool add_pending(const ActorAddress& target, const bytes& data);

    NodeId remote_node_id_;
    PoolConfig config_;
    TlsContext* tls_context_;
    EventLoop* loop_;

    std::vector<TlsConnectionPtr> active_connections_;
    std::deque<PendingMessage> pending_messages_;

    std::atomic<size_t> next_index_{0};
    std::atomic<size_t> reconnect_attempts_{0};
    std::atomic<bool> reconnect_scheduled_{false};

    mutable std::mutex mutex_;
    std::atomic<bool> connecting_{false};
    std::atomic<bool> shutting_down_{false};

    rpc_response_handler rpc_handler_;
};

} // namespace net
} // namespace hpactor