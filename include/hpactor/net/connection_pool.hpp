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

#include <hpactor/actor/spawn.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/endpoint_circuit_breaker.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/tls_connection.hpp>
#include <hpactor/net/tls_context.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/net/wireframe_connection.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/rpc/rpc_types.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace hpactor {

namespace net {

struct WireFrame; // forward decl, full def in <hpactor/net/frame.hpp>

// -----------------------------------------------------------------------------
// PoolConfig - connection pool configuration
// -----------------------------------------------------------------------------
struct PoolConfig {
    size_t min_connections = 1;
    size_t max_connections = 4;
    size_t max_attempts = 5;
    std::chrono::milliseconds initial_backoff{1000};
    std::chrono::milliseconds max_backoff{16000};
    bool use_tls = false; // Default to plain text
    EndpointOutboundLimits outbound_limits{};
    EndpointCircuitBreakerConfig circuit_breaker_cfg{};
};

// Connection pool statistics
struct PoolStats {
    size_t active_connections = 0;
    size_t pending_messages = 0;
    size_t reconnect_attempts = 0;
    bool is_connected = false;
    size_t pending_control_messages = 0;
    size_t pending_data_messages = 0;
    size_t pending_bytes = 0;
    uint8_t pressure_state = 0;
    uint8_t circuit_state = 0;
};

// -----------------------------------------------------------------------------
// ConnectionPool - manages multiple connections to a remote node
// -----------------------------------------------------------------------------
// Owns a collection of PlainConnection/TlsConnection instances for load-
// balanced communication with a single remote endpoint. Handles reconnection
// with exponential backoff and pending message queuing.
// -----------------------------------------------------------------------------
class ConnectionPool {
  public:
    ConnectionPool(EndPoint remote_endpoint, const PoolConfig& config,
                   EventLoop* loop);
    ~ConnectionPool();

    // Non-copyable
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // Send message to specific actor on remote node (uses pool)
    void send(const ActorAddress& target, const StreamBuffer& encoded);

    // Try to send message — returns TransportSendResult describing whether
    // the frame was queued or why it was rejected.
    TransportSendResult
    try_send(const ActorAddress& target, const StreamBuffer& encoded);

    // Send raw bytes to the remote node (uses default target)
    void send(const StreamBuffer& data);

    // Close all connections and clear pending
    void close();

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
    EndPoint remote_endpoint() const {
        return remote_endpoint_;
    }

    // Set handler for RPC responses (called when RpcResponse frame is received)
    using rpc_response_handler = std::function<void(const RpcResponseFrame&)>;
    void set_rpc_handler(rpc_response_handler handler);

    // Set handler for spawn responses (called when SpawnResponse frame is
    // received)
    using spawn_response_handler =
        std::function<void(uint64_t message_id, const SpawnResponse&)>;
    void set_spawn_handler(spawn_response_handler handler);

    // Set handler for actor messages (called for non-RPC, non-spawn frames)
    using actor_message_handler = std::function<void(const WireFrame& frame)>;

    void set_actor_message_handler(actor_message_handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        actor_message_handler_ = std::move(handler);
    }

    // Called by TcpTransport when connection becomes ready
    void on_connection_ready(ConnectionPtr conn);

    // Called by TcpTransport when connection has error
    void on_connection_error(ConnectionPtr conn, const error& err);

    // Handle incoming frame (called by connection's frame handler).
    void on_frame_received(StreamBuffer frame_data);

    // Add an externally-created connection to the pool
    void add_connection(ConnectionPtr conn);

    // Proactively ensure the pool structure exists for an endpoint.
    // The actual connection is established asynchronously on first use.
    // This just ensures the pool is ready so the first send() doesn't pay
    // discovery cost.
    void prewarm_pool(EndPoint ep, const std::vector<AcceptorInfo>& acceptors);

    // Getter for outbound queue (read-only access)
    const EndpointOutboundQueue& outbound_queue() const {
        return outbound_queue_;
    }

    // Getter for circuit breaker (mutable access for reset)
    EndpointCircuitBreaker& circuit_breaker() {
        return circuit_breaker_;
    }

    // Getter for circuit breaker (read-only access)
    const EndpointCircuitBreaker& circuit_breaker() const {
        return circuit_breaker_;
    }

    /// \brief Set the metrics ring buffer for metric emission.
    ///
    /// \param[in] buf Pointer to the system-wide metrics ring buffer.
    void
    set_metrics_ring_buffer(metrics::MpscRingBuffer<metrics::MetricEvent>* buf) {
        metrics_ring_buffer_ = buf;
    }

  private:
    // Get connection via round-robin
    ConnectionPtr get_connection();

    /// \brief Pack the remote endpoint into an ActorId for metric event
    /// transport.
    ///
    /// For IPv4: packs (addr << 16) | port_nw into the ActorId value.
    /// For IPv6: uses a hash of the address combined with the port.
    ///
    /// \return ActorId encoding the endpoint identity.
    [[nodiscard]] ActorId pack_endpoint_for_metrics() const;

    // Schedule reconnect with backoff
    void schedule_reconnect();

    // Flush pending messages
    void flush_pending();

    EndPoint remote_endpoint_;
    PoolConfig config_;
    EventLoop* loop_;

    std::vector<ConnectionPtr> active_connections_;
    EndpointOutboundQueue outbound_queue_;
    EndpointCircuitBreaker circuit_breaker_;

    std::atomic<size_t> next_index_{0};
    std::atomic<size_t> reconnect_attempts_{0};
    std::atomic<bool> reconnect_scheduled_{false};

    mutable std::mutex mutex_;
    std::atomic<bool> shutting_down_{false};

    rpc_response_handler rpc_handler_;
    spawn_response_handler spawn_handler_;
    actor_message_handler actor_message_handler_;

    // Acceptor info for future connection establishment (set via prewarm_pool).
    std::vector<AcceptorInfo> acceptors_;

    // Metrics ring buffer (optional, set via set_metrics_ring_buffer).
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_ = nullptr;
};

} // namespace net
} // namespace hpactor
