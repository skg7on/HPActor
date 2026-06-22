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

/// \brief Connection pool configuration.
struct PoolConfig {
    /// \brief Minimum number of connections to maintain (default 1).
    size_t min_connections = 1;
    /// \brief Maximum number of connections allowed (default 4).
    size_t max_connections = 4;
    /// \brief Maximum reconnect attempts before failing (default 5).
    size_t max_attempts = 5;
    /// \brief Initial reconnect backoff (default 1 s).
    std::chrono::milliseconds initial_backoff{1000};
    /// \brief Maximum reconnect backoff (default 16 s).
    std::chrono::milliseconds max_backoff{16000};
    /// \brief Whether to use TLS for connections (default false).
    bool use_tls = false;
    /// \brief Outbound queue limits for this pool.
    EndpointOutboundLimits outbound_limits{};
    /// \brief Circuit breaker configuration for this pool.
    EndpointCircuitBreakerConfig circuit_breaker_cfg{};
};

/// \brief Connection pool runtime statistics.
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

/// \brief Manages multiple connections to a single remote node.
///
/// Owns a collection of \c PlainConnection / \c TlsConnection instances
/// for load-balanced communication with a single remote endpoint.
/// Handles reconnection with exponential backoff, pending message
/// queuing, outbound admission control via \c EndpointOutboundQueue,
/// and circuit breaking via \c EndpointCircuitBreaker.
///
/// \note Thread safety: \c try_send(), \c send(), \c stats(), and
///       \c is_connected() may be called from any thread. Other methods
///       are called from the event loop thread.
class ConnectionPool {
  public:
    /// \brief Construct a connection pool for a remote endpoint.
    ///
    /// \param[in] remote_endpoint Target node endpoint.
    /// \param[in] config Pool configuration.
    /// \param[in] loop Owning event loop.
    ConnectionPool(EndPoint remote_endpoint, const PoolConfig& config,
                   EventLoop* loop);
    ~ConnectionPool();

    /// \name Non-copyable
    /// @{
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    /// @}

    /// \brief Send a message to a specific actor on the remote node.
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] encoded Serialized message payload.
    void send(const ActorAddress& target, const StreamBuffer& encoded);

    /// \brief Try to send a message — returns whether it was queued or
    /// rejected.
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] encoded Serialized message payload.
    /// \return Result describing whether the frame was queued or why it
    ///         was rejected.
    TransportSendResult
    try_send(const ActorAddress& target, const StreamBuffer& encoded);

    /// \brief Send raw bytes to the remote node (no actor routing).
    ///
    /// \param[in] data Raw bytes to send.
    void send(const StreamBuffer& data);

    /// \brief Close all connections and clear pending messages.
    void close();

    /// \brief Check whether the pool has any active connections.
    ///
    /// \return \c true if at least one connection is established.
    bool is_connected() const;

    /// \brief Collect pool statistics for observability.
    ///
    /// \return Snapshot of current pool state.
    PoolStats stats() const;

    /// \brief Graceful shutdown: drain pending messages.
    ///
    /// Waits for in-flight messages to complete.
    /// \return Number of messages that could not be sent.
    size_t drain();

    /// \brief Immediate shutdown without draining.
    void abort();

    /// \brief Return the remote node endpoint.
    ///
    /// \return Target endpoint for this pool.
    EndPoint remote_endpoint() const {
        return remote_endpoint_;
    }

    /// \brief Handler for incoming RPC response frames.
    ///
    /// \param[in] frame The RPC response frame.
    using rpc_response_handler = std::function<void(const RpcResponseFrame&)>;
    void set_rpc_handler(rpc_response_handler handler);

    /// \brief Handler for incoming spawn response frames.
    ///
    /// \param[in] message_id Correlation message ID.
    /// \param[in] response Spawn response.
    using spawn_response_handler =
        std::function<void(uint64_t message_id, const SpawnResponse&)>;
    void set_spawn_handler(spawn_response_handler handler);

    /// \brief Handler for incoming actor messages (non-RPC, non-spawn
    /// frames).
    ///
    /// \param[in] frame The wire frame.
    using actor_message_handler = std::function<void(const WireFrame& frame)>;

    void set_actor_message_handler(actor_message_handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        actor_message_handler_ = std::move(handler);
    }

    /// \brief Called by \c TcpTransport when a connection becomes ready.
    ///
    /// \param[in] conn The newly-ready connection.
    void on_connection_ready(ConnectionPtr conn);

    /// \brief Called by \c TcpTransport when a connection errors.
    ///
    /// \param[in] conn The failed connection.
    /// \param[in] err Error details.
    void on_connection_error(ConnectionPtr conn, const error& err);

    /// \brief Handle an incoming frame from a connection.
    ///
    /// \param[in] frame_data Serialized wire frame payload.
    void on_frame_received(StreamBuffer frame_data);

    /// \brief Add an externally-created connection to the pool.
    ///
    /// \param[in] conn The connection to add.
    void add_connection(ConnectionPtr conn);

    /// \brief Proactively ensure the pool structure exists for an endpoint.
    ///
    /// The actual connection is established asynchronously on first use;
    /// this just ensures the pool is ready so the first \c send() does not
    /// pay discovery cost.
    /// \param[in] ep Endpoint to prewarm for.
    /// \param[in] acceptors Acceptor advertisements for this endpoint.
    void prewarm_pool(EndPoint ep, const std::vector<AcceptorInfo>& acceptors);

    /// \brief Read-only access to the outbound queue.
    const EndpointOutboundQueue& outbound_queue() const {
        return outbound_queue_;
    }

    /// \brief Mutable access to the circuit breaker (for reset).
    EndpointCircuitBreaker& circuit_breaker() {
        return circuit_breaker_;
    }

    /// \brief Read-only access to the circuit breaker.
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
    ConnectionPtr get_connection();

    /// \brief Pack the remote endpoint into an ActorId for metric event
    /// transport.
    ///
    /// For IPv4: packs \c (addr << 16) | port_nw into the ActorId value.
    /// For IPv6: uses a hash of the address combined with the port.
    /// \return ActorId encoding the endpoint identity.
    [[nodiscard]] ActorId pack_endpoint_for_metrics() const;

    void schedule_reconnect();
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

    std::vector<AcceptorInfo> acceptors_;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_ = nullptr;
};

} // namespace net
} // namespace hpactor
