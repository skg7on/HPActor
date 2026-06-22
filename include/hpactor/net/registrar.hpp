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

#include <hpactor/adt/node_identity.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {

namespace net {

/// \brief A single static route entry for registrar configuration.
struct StaticRouteConfig {
    /// \brief Logical endpoint of the target node.
    EndPoint endpoint;
    /// \brief IP address or DNS hostname (used if \c endpoint is empty).
    std::string address;
    /// \brief TCP port of the target node.
    uint16_t port = 0;
};

/// \brief Configuration for the UDP/TCP registrar subsystem.
struct RegistrarConfig {
    /// \brief UDP port for discovery queries (default 5353).
    uint16_t udp_port = 5353;
    /// \brief TCP port for the registrar server (default 5353).
    uint16_t tcp_port = 5353;
    /// \brief Heartbeat interval for connected clients (default 5 s).
    std::chrono::milliseconds heartbeat_interval{5000};
    /// \brief Time before an unresponsive client is expired (default 15 s).
    std::chrono::milliseconds expiration_timeout{15000};
    /// \brief Probe interval for liveness checks (default 30 s).
    std::chrono::milliseconds probe_interval{30000};
    /// \brief Pre-configured static routes.
    std::vector<StaticRouteConfig> static_routes;
    /// \brief Whether to disable the local registrar server.
    bool disable_server = false;
};

/// \brief Information about a known node in the registry.
struct NodeEndpoint {
    /// \brief Node identity (endpoint, host, acceptors).
    NodeIdentity identity;
    /// \brief TCP port for actor communication.
    uint16_t tcp_port = 0;
    /// \brief Whether this entry comes from a static route.
    bool is_static_route = false;
    /// \brief Timestamp of the last update from this node.
    std::chrono::steady_clock::time_point last_seen;
};

/// \brief Hostname-to-IP resolution with in-memory caching.
///
/// Provides synchronous and asynchronous hostname resolution with
/// TTL-based cache expiry.
///
/// \note Thread safety: All public methods are safe to call from any
///       thread. Internal synchronization uses a mutex.
class HostResolver {
  public:
    HostResolver() = default;

    /// \brief Resolve a hostname to an IP address (blocking).
    ///
    /// \param[in] hostname DNS hostname to resolve.
    /// \return IP address string, or empty string on failure.
    std::string resolve(const std::string& hostname);

    /// \brief Asynchronously resolve a hostname.
    ///
    /// Returns immediately; the callback is invoked when resolution
    /// completes.
    /// \param[in] hostname DNS hostname to resolve.
    /// \param[in] callback Invoked with the resolved IP (or empty string).
    void resolve_async(const std::string& hostname,
                       std::function<void(std::string ip)> callback);

    /// \brief Look up a cached IP for a hostname.
    ///
    /// \param[in] hostname Hostname to search for.
    /// \return Cached IP, or empty string if not found or expired.
    std::string get_cached(const std::string& hostname) const;

    /// \brief Cache a hostname-to-IP mapping.
    ///
    /// \param[in] hostname Hostname.
    /// \param[in] ip Resolved IP address.
    /// \param[in] ttl Cache time-to-live (default 300 s).
    void cache(const std::string& hostname, const std::string& ip,
               std::chrono::seconds ttl = std::chrono::seconds(300));

    /// \brief Remove all expired cache entries.
    void clear_expired();

  private:
    struct CacheEntry {
        std::string ip;
        std::chrono::steady_clock::time_point expires_at;
    };

    std::unordered_map<std::string, CacheEntry> cache_;
    mutable std::mutex mutex_;
};

/// \brief Registry of known nodes with expiry and static-route support.
///
/// Stores \c NodeEndpoint entries keyed by \c EndPoint. Supports
/// upsert, lookup, and automatic expiry of stale entries.
///
/// \note Thread safety: All public methods are safe to call from any
///       thread. Internal synchronization uses a mutex.
class NodeRegistry {
  public:
    /// \brief Construct with registrar configuration.
    ///
    /// \param[in] config Registrar configuration (used for expiry).
    explicit NodeRegistry(const RegistrarConfig& config);

    /// \brief Add or update an endpoint in the registry.
    ///
    /// \param[in] endpoint Node endpoint to upsert.
    void upsert_endpoint(NodeEndpoint endpoint);

    /// \brief Remove an endpoint from the registry.
    ///
    /// \param[in] endpoint Endpoint to remove.
    /// \return \c true if the endpoint was found and removed.
    bool remove_endpoint(EndPoint endpoint);

    /// \brief Look up a node by endpoint.
    ///
    /// \param[in] endpoint Endpoint to search for.
    /// \return Pointer to the node, or \c nullptr if not found.
    /// \note The returned pointer is invalidated by the next write to
    ///       the registry.
    NodeEndpoint* get(EndPoint endpoint);

    /// \brief Check whether an endpoint exists in the registry.
    ///
    /// \param[in] endpoint Endpoint to check.
    /// \return \c true if the endpoint is registered.
    bool has(EndPoint endpoint) const;

    /// \brief Return all registered endpoints.
    ///
    /// \return Copy of the current endpoint list.
    std::vector<NodeEndpoint> all() const;

    /// \brief Remove all expired entries.
    ///
    /// \return Number of entries removed.
    size_t remove_expired();

  private:
    RegistrarConfig config_;
    std::unordered_map<EndPoint, NodeEndpoint> endpoints_;
    mutable std::mutex mutex_;
};

/// \brief UDP/TCP registrar protocol message types.
enum class RegistrarMessageType : uint8_t {
    Register = 0x01,        ///< TCP: client registration request.
    Heartbeat = 0x02,       ///< TCP: keepalive heartbeat.
    NodeJoin = 0x03,        ///< TCP: broadcast that a node joined.
    NodeLeave = 0x04,       ///< TCP: broadcast that a node left.
    Accept = 0x05,          ///< TCP: server acceptance response.
    Error = 0x06,           ///< TCP: error response.
    ResolveQuery = 0x10,    ///< UDP: endpoint resolution query.
    ResolveResponse = 0x11, ///< UDP: endpoint resolution response.
};

/// \brief Magic number for the UDP registrar protocol ("HPAC").
constexpr uint32_t RegistrarMagic = 0x48504143;
/// \brief Registrar wire protocol version.
constexpr uint8_t RegistrarVersion = 0x01;
/// \brief Size of the UDP registrar message header.
constexpr size_t RegistrarHeaderSize = 12;

/// \brief Payload for node announce/join broadcast messages.
struct NodeAnnouncePayload {
    uint16_t tcp_port;
    uint16_t actor_count;
};

/// \brief Payload for endpoint resolution queries.
struct NodeQueryPayload {
    EndPoint target_endpoint;
};

/// \brief Payload for endpoint resolution responses.
struct NodeResponsePayload {
    uint16_t tcp_port;
};

/// \brief Payload for liveness probe messages.
struct NodeProbePayload {
    uint64_t probe_id;
    uint64_t timestamp;
};

/// \brief Magic number for the TCP registrar protocol ("HPRC").
constexpr uint32_t TcpRegistrarMagic = 0x48505243;
/// \brief TCP registrar wire protocol version.
constexpr uint8_t TcpRegistrarVersion = 0x01;
/// \brief Size of the TCP registrar message header.
constexpr size_t TcpHeaderSize = 10;

/// \brief TCP registrar message types (separate from UDP types).
enum class TcpMessageType : uint8_t {
    Register = 0x01,  ///< Client registration.
    Heartbeat = 0x02, ///< Keepalive heartbeat.
    NodeJoin = 0x03,  ///< Broadcast: node joined.
    NodeLeave = 0x04, ///< Broadcast: node left.
    Accept = 0x05,    ///< Server acceptance.
    Error = 0x06,     ///< Error response.
};

/// \brief Error codes for TCP registrar error messages.
enum class RegistrarError : uint8_t {
    None = 0,           ///< No error.
    NameTaken = 1,      ///< Node identity already registered.
    InvalidMessage = 2, ///< Malformed or unexpected message.
};

// Forward declarations
class RegistrarServer;
class RegistrarClient;
class NodeRegistry;

class RegistrarConnection;
using RegistrarConnectionPtr = std::shared_ptr<RegistrarConnection>;

/// \brief Async TCP connection for the registrar protocol.
///
/// Handles the binary registrar wire format over a TCP socket with
/// edge-triggered read handling and write buffering. Can be created
/// from an accepted server socket or as an outbound client connection.
///
/// \note Thread safety: Called from the event loop thread.
class RegistrarConnection
    : public std::enable_shared_from_this<RegistrarConnection> {
    friend class RegistrarServer;

  public:
    /// \brief Callback for received messages.
    ///
    /// \param[in] type TCP message type.
    /// \param[in] data Message payload.
    using message_handler =
        std::function<void(TcpMessageType, const StreamBuffer&)>;

    /// \brief Callback invoked when the connection is closed.
    using disconnect_handler = std::function<void()>;

    /// \brief Callback invoked when an async send completes.
    ///
    /// \param[in] result Byte count or negative errno.
    using send_complete_handler = std::function<void(int result)>;

    /// \brief Create from an accepted server socket.
    ///
    /// \param[in] fd Accepted client file descriptor.
    /// \param[in] remote_endpoint Remote address.
    /// \param[in] loop Owning event loop.
    /// \return Shared pointer to the new connection.
    static RegistrarConnectionPtr
    accepted(int fd, EndPoint remote_endpoint, EventLoop* loop);

    /// \brief Create as an outbound client connection.
    ///
    /// \param[in] fd Connected client file descriptor.
    /// \param[in] remote_endpoint Server address.
    /// \param[in] loop Owning event loop.
    /// \return Shared pointer to the new connection.
    static RegistrarConnectionPtr
    connecting(int fd, EndPoint remote_endpoint, EventLoop* loop);

    ~RegistrarConnection();

    /// \brief Set the message handler.
    ///
    /// \param[in] h Callback invoked for each complete message.
    void set_message_handler(message_handler h);

    /// \brief Set the disconnect handler.
    ///
    /// \param[in] h Callback invoked on connection close.
    void set_disconnect_handler(disconnect_handler h);

    /// \brief Set the send-completion handler.
    ///
    /// \param[in] h Callback invoked when an async send finishes.
    void set_send_complete_handler(send_complete_handler h);

    /// \brief Send a registrar protocol message.
    ///
    /// \param[in] type TCP message type.
    /// \param[in] payload Serialized message payload.
    void send_message(TcpMessageType type, const StreamBuffer& payload);

    /// \brief Close the connection.
    void close();

    /// \brief Return the remote endpoint.
    EndPoint remote_endpoint() const {
        return remote_endpoint_;
    }

    /// \brief Return the file descriptor.
    int fd() const {
        return fd_;
    }

  private:
    enum class ReadState { ReadingHeader, ReadingPayload };

    RegistrarConnection(EndPoint remote_endpoint, EventLoop* loop, int fd);

    void register_with_loop();
    void handle_read_event();
    void handle_payload_read();
    void flush_write_buffer();
    void handle_send_completion(int result);

    EndPoint remote_endpoint_;
    EventLoop* loop_ = nullptr;
    int fd_ = -1;

    ReadState read_state_ = ReadState::ReadingHeader;
    size_t header_bytes_read_ = 0;
    StreamBuffer header_buffer_;

    TcpMessageType current_type_ = TcpMessageType::Register;
    size_t payload_bytes_read_ = 0;
    StreamBuffer payload_buffer_;

    StreamBuffer write_buffer_;
    bool is_sending_ = false;

    message_handler message_handler_;
    disconnect_handler disconnect_handler_;
    send_complete_handler send_complete_handler_;
};

/// \brief TCP-based authoritative registrar server.
///
/// Accepts TCP connections from registrar clients, maintains a
/// \c NodeRegistry of all known nodes, and broadcasts join/leave
/// events to all connected clients.
///
/// \note Thread safety: Called from the event loop thread.
class RegistrarServer {
  public:
    /// \brief Construct a registrar server.
    ///
    /// \param[in] config Registrar configuration.
    /// \param[in] local_endpoint This server's endpoint.
    /// \param[in] loop Event loop for async I/O (can be set later).
    RegistrarServer(const RegistrarConfig& config, EndPoint local_endpoint,
                    EventLoop* loop = nullptr);
    ~RegistrarServer();

    /// \name Non-copyable
    /// @{
    RegistrarServer(const RegistrarServer&) = delete;
    RegistrarServer& operator=(const RegistrarServer&) = delete;
    /// @}

    /// \brief Start the TCP server and begin accepting connections.
    void start();

    /// \brief Stop the server and disconnect all clients.
    void stop();

    /// \brief Access the node registry.
    ///
    /// \return Pointer to the internal registry (lifetime matches the server).
    NodeRegistry* registry() {
        return &registry_;
    }

    /// \brief Set the event loop (must be called before \c start()).
    ///
    /// \param[in] loop Event loop for async I/O.
    void set_event_loop(EventLoop* loop) {
        loop_ = loop;
    }

    /// \brief Handle an accepted TCP connection.
    ///
    /// Called by the acceptor on each new client.
    /// \param[in] client_fd Accepted client file descriptor.
    /// \param[in] remote_endpoint Remote client address.
    void handle_accept(int client_fd, EndPoint remote_endpoint);

    /// \brief Broadcast a node-joined event to all clients.
    ///
    /// \param[in] endpoint Endpoint of the joined node.
    /// \param[in] ep Node endpoint details.
    void broadcast_node_joined(EndPoint endpoint, const NodeEndpoint& ep);

    /// \brief Broadcast a node-left event to all clients.
    ///
    /// \param[in] endpoint Endpoint of the departed node.
    void broadcast_node_left(EndPoint endpoint);

  private:
    void handle_tcp_message(RegistrarConnectionPtr conn, TcpMessageType type,
                            const StreamBuffer& data);
    void handle_disconnect(RegistrarConnectionPtr conn);

    RegistrarConfig config_;
    [[maybe_unused]] EndPoint local_endpoint_;
    NodeRegistry registry_;
    EventLoop* loop_ = nullptr;
    TcpAcceptor acceptor_;

    std::atomic<bool> running_{false};

    // Connected clients (endpoint -> connection)
    std::unordered_map<EndPoint, RegistrarConnectionPtr> clients_;
    // fd -> connection map for completion routing
    std::unordered_map<int, RegistrarConnectionPtr> fd_to_connection_;
    std::mutex clients_mutex_;
};

/// \brief Dual-mode registrar implementing \c IServiceDiscovery.
///
/// Operates in either server mode (binds a UDP socket and runs a
/// \c RegistrarServer for authoritative discovery) or client mode
/// (connects to a registrar server for discovery). Mode is determined
/// by bind success/failure.
///
/// \note Thread safety: Member access is synchronized internally.
class UdpRegistrar : public IServiceDiscovery {
  public:
    /// \brief Construct a UDP registrar.
    ///
    /// \param[in] config Registrar configuration.
    /// \param[in] local_endpoint This node's endpoint.
    /// \param[in] loop Event loop for async I/O (can be set later).
    UdpRegistrar(const RegistrarConfig& config, EndPoint local_endpoint,
                 EventLoop* loop = nullptr);
    ~UdpRegistrar();

    /// \name Non-copyable
    /// @{
    UdpRegistrar(const UdpRegistrar&) = delete;
    UdpRegistrar& operator=(const UdpRegistrar&) = delete;
    /// @}

    /// \brief Set the event loop (must be called before \c start()).
    ///
    /// \param[in] loop Event loop for async I/O.
    void set_event_loop(EventLoop* loop) {
        loop_ = loop;
    }

    /// \brief Start discovery — determines server vs client mode.
    void start() override;

    /// \brief Stop discovery and release resources.
    void stop() override;

    /// \brief Look up a node endpoint in the registry.
    ///
    /// \param[in] endpoint Endpoint to search for.
    /// \return Pointer to the node endpoint, or \c nullptr.
    NodeEndpoint* get_endpoint(EndPoint endpoint);

    /// \brief Return all known endpoints.
    ///
    /// \return Copy of the current endpoint list.
    std::vector<NodeEndpoint> get_all_endpoints() const;

    /// \brief Callback for node online/offline events.
    ///
    /// \param[in] ep Endpoint of the node.
    /// \param[in] online \c true if online, \c false if offline.
    using node_callback = std::function<void(EndPoint, bool online)>;

    /// \brief Register a node status callback.
    ///
    /// \param[in] cb Callback invoked on node status changes.
    void set_node_callback(node_callback cb);

    /// \brief Handle an incoming UDP packet.
    ///
    /// \param[in] data Packet payload.
    /// \param[in] from_host Source hostname/IP.
    /// \param[in] from_port Source port.
    void handle_udp_packet(const StreamBuffer& data,
                           const std::string& from_host, uint16_t from_port);

    // ── IServiceDiscovery overrides ────────────────────────────────────
    std::vector<Member> discover_all() const override;
    const Member* discover(EndPoint ep) const override;
    void announce(Member m) override;
    void on_member_change(MemberChangeCallback cb) override;
    std::string backend_name() const override {
        return "udp-registrar";
    }
    const std::unordered_map<EndPoint, Member>* raw_members() const override {
        return &endpoint_to_member_;
    }

  private:
    void start_server_mode();
    void start_client_mode();
    void start_server_mode_async();
    void start_client_mode_async();
    void setup_udp_socket();
    void issue_async_recvfrom();
    void handle_udp_read_ready();
    void
    handle_udp_recv_completion(const StreamBuffer& data,
                               const std::string& from_host, uint16_t from_port);
    void
    send_udp_response(const StreamBuffer& data, const struct sockaddr_in& dest);
    void handle_resolve_query(const StreamBuffer& payload,
                              const std::string& from_host, uint16_t from_port);
    void handle_resolve_response(const StreamBuffer& payload);
    void send_resolve_response(const NodeEndpoint& endpoint,
                               const std::string& from_host,
                               uint16_t from_port) const;
    void failover();

    // UDP receive state
    static constexpr size_t kUdpRecvBufferSize = 65536;
    StreamBuffer udp_recv_buffer_;
    struct sockaddr_in udp_src_addr_;
    socklen_t udp_src_addr_len_ = sizeof(udp_src_addr_);

    RegistrarConfig config_;
    EndPoint local_endpoint_;
    EventLoop* loop_ = nullptr;

    // Either server or client (not both)
    std::unique_ptr<RegistrarServer> server_;
    std::unique_ptr<RegistrarClient> client_;

    // Client mode registry (populated from static routes)
    std::unique_ptr<NodeRegistry> client_registry_;

    // UDP socket for sending resolution responses
    int udp_socket_ = -1;

    node_callback node_callback_;

    // IServiceDiscovery state
    static Member to_member(const NodeEndpoint& ep);
    mutable std::unordered_map<EndPoint, Member> endpoint_to_member_;
    MemberChangeCallback member_change_cb_;
};

/// \brief TCP client for connecting to a \c RegistrarServer.
///
/// Handles registration, heartbeat keepalive, and failover to alternate
/// servers. Shares a \c NodeRegistry with the owning \c UdpRegistrar.
///
/// \note Thread safety: Called from the event loop thread.
class RegistrarClient {
  public:
    /// \brief Construct a registrar client.
    ///
    /// \param[in] config Registrar configuration.
    /// \param[in] local_endpoint This client's endpoint.
    /// \param[in] server_endpoint Target server endpoint.
    /// \param[in] shared_registry Shared registry for endpoint updates
    ///            (not owned).
    /// \param[in] loop Event loop for async I/O (can be set later).
    RegistrarClient(const RegistrarConfig& config, EndPoint local_endpoint,
                    EndPoint server_endpoint, NodeRegistry* shared_registry,
                    EventLoop* loop = nullptr);
    ~RegistrarClient();

    /// \name Non-copyable
    /// @{
    RegistrarClient(const RegistrarClient&) = delete;
    RegistrarClient& operator=(const RegistrarClient&) = delete;
    /// @}

    /// \brief Start the client — connect and register.
    void start();

    /// \brief Stop the client and disconnect.
    void stop();

    /// \brief Set the event loop (must be called before \c start()).
    ///
    /// \param[in] loop Event loop for async I/O.
    void set_event_loop(EventLoop* loop) {
        loop_ = loop;
    }

    /// \brief Set acceptors to announce during registration.
    ///
    /// \param[in] acceptors List of acceptor advertisements.
    void set_acceptors(std::vector<AcceptorInfo> acceptors);

    /// \brief Set a callback for repeated reconnect failures.
    ///
    /// Invoked after \c kMaxReconnectAttempts consecutive failures.
    /// \param[in] cb Callback for failover handling.
    void set_failover_callback(std::function<void()> cb) {
        failover_callback_ = std::move(cb);
    }

    /// \brief Reconnect to the server after a disconnect.
    void reconnect();

    /// \brief Check whether the client is connected.
    ///
    /// \return \c true if an active connection exists.
    bool is_connected() const {
        return connected_.load();
    }

  private:
    void attempt_connection();
    void send_registration();

    // Handle server messages
    void handle_server_message(TcpMessageType type, const StreamBuffer& data);
    void handle_disconnect();

    RegistrarConfig config_;
    EndPoint local_endpoint_;
    EndPoint server_endpoint_;
    NodeRegistry* shared_registry_; // Not owned
    EventLoop* loop_ = nullptr;

    RegistrarConnectionPtr server_connection_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    // Timer handles
    uint64_t heartbeat_timer_ = 0;

    // For heartbeat tracking
    std::chrono::steady_clock::time_point last_heartbeat_sent_;

    // Acceptors announced during registration
    std::vector<AcceptorInfo> acceptors_;

    // Failover support
    static constexpr int kMaxReconnectAttempts = 5;
    int reconnect_attempts_ = 0;
    std::function<void()> failover_callback_;
};

} // namespace net
} // namespace hpactor
