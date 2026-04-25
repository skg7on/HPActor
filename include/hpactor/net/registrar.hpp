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
#include <hpactor/net/event_loop.hpp>
#include <hpactor/types/types.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// AcceptorInfo - information about a server acceptor
// -----------------------------------------------------------------------------
struct AcceptorInfo {
    uint16_t port = 0;
    uint8_t handshake_version = 0;
    uint8_t protocol_version = 0;
    bool tls_required = false;
};

// -----------------------------------------------------------------------------
// RegistrarConfig - configuration for registrar
// -----------------------------------------------------------------------------
struct StaticRouteConfig {
    CommunicationEndpoint endpoint;
    std::string address;     // IP or DNS hostname (used if endpoint is empty)
    uint16_t port = 0;
};

struct RegistrarConfig {
    uint16_t udp_port = 5353;
    uint16_t tcp_port = 5353;
    std::chrono::milliseconds heartbeat_interval{5000};
    std::chrono::milliseconds expiration_timeout{15000};
    std::chrono::milliseconds probe_interval{30000};
    std::vector<StaticRouteConfig> static_routes;
    bool disable_server = false;
};

// -----------------------------------------------------------------------------
// NodeEndpoint - information about a known node
// -----------------------------------------------------------------------------
struct NodeEndpoint {
    CommunicationEndpoint endpoint;
    std::string host;        // Resolved IP or hostname
    uint16_t tcp_port = 0;
    bool is_static_route = false;
    std::vector<AcceptorInfo> acceptors;
    std::chrono::steady_clock::time_point last_seen;
    std::string uds_path;     // NEW: path to UDS socket, empty if not available
};

// -----------------------------------------------------------------------------
// HostResolver - hostname to IP resolution with caching
// -----------------------------------------------------------------------------
class HostResolver {
public:
    HostResolver() = default;

    // Resolve hostname to IP address (blocking)
    std::string resolve(const std::string& hostname);

    // Async resolution - returns immediately, callback when done
    void resolve_async(const std::string& hostname,
                       std::function<void(std::string ip)> callback);

    // Get cached IP for hostname (empty if not cached)
    std::string get_cached(const std::string& hostname) const;

    // Cache hostname -> IP mapping with TTL
    void cache(const std::string& hostname, const std::string& ip,
               std::chrono::seconds ttl = std::chrono::seconds(300));

    // Clear expired entries
    void clear_expired();

private:
    struct CacheEntry {
        std::string ip;
        std::chrono::steady_clock::time_point expires_at;
    };

    std::unordered_map<std::string, CacheEntry> cache_;
    mutable std::mutex mutex_;
};

// -----------------------------------------------------------------------------
// NodeRegistry - registry of known nodes
// -----------------------------------------------------------------------------
class NodeRegistry {
public:
    explicit NodeRegistry(const RegistrarConfig& config);

    // Add or update an endpoint
    void upsert_endpoint(NodeEndpoint endpoint);

    // Remove an endpoint
    bool remove_endpoint(CommunicationEndpoint endpoint);

    // Get endpoint (nullptr if not found)
    NodeEndpoint* get(CommunicationEndpoint endpoint);

    // Check if endpoint exists
    bool has(CommunicationEndpoint endpoint) const;

    // Get all endpoints
    std::vector<NodeEndpoint> all() const;

    // Remove expired entries
    size_t remove_expired();

private:
    RegistrarConfig config_;
    std::unordered_map<CommunicationEndpoint, NodeEndpoint> endpoints_;
    mutable std::mutex mutex_;
};

// -----------------------------------------------------------------------------
// Registrar Protocol Messages
// -----------------------------------------------------------------------------
enum class RegistrarMessageType : uint8_t {
    // TCP messages (server/client registration)
    Register = 0x01,
    Heartbeat = 0x02,
    NodeJoin = 0x03,
    NodeLeave = 0x04,
    Accept = 0x05,
    Error = 0x06,
    // UDP messages (resolution)
    ResolveQuery = 0x10,
    ResolveResponse = 0x11,
};

// Protocol constants
constexpr uint32_t RegistrarMagic = 0x48504143;  // "HPAC"
constexpr uint8_t RegistrarVersion = 0x01;
constexpr size_t RegistrarHeaderSize = 12;

// Message payloads
struct NodeAnnouncePayload {
    uint16_t tcp_port;
    uint16_t actor_count;
};

struct NodeQueryPayload {
    CommunicationEndpoint target_endpoint;
};

struct NodeResponsePayload {
    uint16_t tcp_port;
};

struct NodeProbePayload {
    uint64_t probe_id;
    uint64_t timestamp;
};

// -----------------------------------------------------------------------------
// TCP Message Framing for RegistrarServer
// -----------------------------------------------------------------------------
constexpr uint32_t TcpRegistrarMagic = 0x48505243;  // "HPRC"
constexpr uint8_t TcpRegistrarVersion = 0x01;
constexpr size_t TcpHeaderSize = 10;

// TCP Message types (different from UDP types)
enum class TcpMessageType : uint8_t {
    Register = 0x01,
    Heartbeat = 0x02,
    NodeJoin = 0x03,
    NodeLeave = 0x04,
    Accept = 0x05,
    Error = 0x06,
};

// Error codes for TCP Error messages
enum class RegistrarError : uint8_t {
    None = 0,
    NameTaken = 1,
    InvalidMessage = 2,
};

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
class RegistrarServer;
class RegistrarClient;
class NodeRegistry;

// Forward declaration
class RegistrarConnection;
using RegistrarConnectionPtr = std::shared_ptr<RegistrarConnection>;

// RegistrarConnection - async TCP connection for registrar protocol
class RegistrarConnection : public std::enable_shared_from_this<RegistrarConnection> {
    friend class RegistrarServer;

public:
    using message_handler = std::function<void(TcpMessageType, const bytes&)>;
    using disconnect_handler = std::function<void()>;
    using send_complete_handler = std::function<void(int result)>;

    // Create from accepted server socket
    static RegistrarConnectionPtr accepted(int fd,
                                          CommunicationEndpoint remote_endpoint,
                                          EventLoop* loop);

    // Create as client connection
    static RegistrarConnectionPtr connecting(int fd,
                                           CommunicationEndpoint remote_endpoint,
                                           EventLoop* loop);

    ~RegistrarConnection();

    // Set handlers
    void set_message_handler(message_handler h);
    void set_disconnect_handler(disconnect_handler h);
    void set_send_complete_handler(send_complete_handler h);

    // Send registrar message
    void send_message(TcpMessageType type, const bytes& payload);

    // Close connection
    void close();

    // Get remote endpoint
    CommunicationEndpoint remote_endpoint() const { return remote_endpoint_; }

    // Get fd
    int fd() const { return fd_; }

private:
    enum class ReadState {
        ReadingHeader,
        ReadingPayload
    };

    RegistrarConnection(CommunicationEndpoint remote_endpoint, EventLoop* loop, int fd);

    void register_with_loop();
    void handle_read_event();
    void handle_payload_read();
    void flush_write_buffer();
    void handle_send_completion(int result);

    CommunicationEndpoint remote_endpoint_;
    EventLoop* loop_ = nullptr;
    int fd_ = -1;

    ReadState read_state_ = ReadState::ReadingHeader;
    size_t header_bytes_read_ = 0;
    bytes header_buffer_;

    TcpMessageType current_type_ = TcpMessageType::Register;
    size_t payload_bytes_read_ = 0;
    bytes payload_buffer_;

    bytes write_buffer_;
    bool is_sending_ = false;

    message_handler message_handler_;
    disconnect_handler disconnect_handler_;
    send_complete_handler send_complete_handler_;
};

// -----------------------------------------------------------------------------
// RegistrarServer - TCP-based authoritative registrar
// -----------------------------------------------------------------------------
class RegistrarServer {
public:
    RegistrarServer(const RegistrarConfig& config, CommunicationEndpoint local_endpoint,
                   EventLoop* loop = nullptr);
    ~RegistrarServer();

    // Non-copyable
    RegistrarServer(const RegistrarServer&) = delete;
    RegistrarServer& operator=(const RegistrarServer&) = delete;

    // Start TCP server and UDP listener
    void start();
    void stop();

    // Get registry for reading
    NodeRegistry* registry() { return &registry_; }

    // Set event loop for async I/O (can be changed before start())
    void set_event_loop(EventLoop* loop) { loop_ = loop; }

    // Handle incoming TCP connection
    void handle_accept(int client_fd, CommunicationEndpoint remote_endpoint);

    // Broadcast event to all connected clients
    void broadcast_node_joined(CommunicationEndpoint endpoint, const NodeEndpoint& ep);
    void broadcast_node_left(CommunicationEndpoint endpoint);

private:
    void handle_tcp_message(RegistrarConnectionPtr conn, TcpMessageType type, const bytes& data);
    void handle_disconnect(RegistrarConnectionPtr conn);

    RegistrarConfig config_;
    [[maybe_unused]] CommunicationEndpoint local_endpoint_;
    NodeRegistry registry_;
    EventLoop* loop_ = nullptr;
    Acceptor acceptor_;

    int udp_socket_ = -1;
    std::atomic<bool> running_{false};

    // Connected clients (endpoint -> connection)
    std::unordered_map<CommunicationEndpoint, RegistrarConnectionPtr> clients_;
    // fd -> connection map for completion routing
    std::unordered_map<int, RegistrarConnectionPtr> fd_to_connection_;
    std::mutex clients_mutex_;

    // Event processing thread
    std::thread event_thread_;
};

// -----------------------------------------------------------------------------
// UdpRegistrar - Dual-mode registrar (server or client)
// -----------------------------------------------------------------------------
class UdpRegistrar {
public:
    UdpRegistrar(const RegistrarConfig& config, CommunicationEndpoint local_endpoint,
                EventLoop* loop = nullptr);
    ~UdpRegistrar();

    // Non-copyable
    UdpRegistrar(const UdpRegistrar&) = delete;
    UdpRegistrar& operator=(const UdpRegistrar&) = delete;

    // Set event loop for async I/O (can be changed before start())
    void set_event_loop(EventLoop* loop) { loop_ = loop; }

    // Start listening - determines server vs client mode based on bind result
    void start();
    void stop();

    // Query endpoint
    NodeEndpoint* get_endpoint(CommunicationEndpoint endpoint);

    // Get all known endpoints
    std::vector<NodeEndpoint> get_all_endpoints() const;

    // Set callback for node online/offline events
    using node_callback = std::function<void(CommunicationEndpoint, bool online)>;
    void set_node_callback(node_callback cb);

    // Handle incoming UDP packet (for resolution)
    void handle_udp_packet(const bytes& data, const std::string& from_host, uint16_t from_port);

private:
    void start_server_mode();
    void start_client_mode();
    void start_server_mode_async();
    void start_client_mode_async();
    void issue_async_recvfrom();
    void handle_udp_read_ready();
    void handle_udp_recv_completion(const bytes& data, const std::string& from_host, uint16_t from_port);
    void send_udp_response(const bytes& data, const struct sockaddr_in& dest);
    void failover();

    // UDP receive state
    static constexpr size_t kUdpRecvBufferSize = 65536;
    bytes udp_recv_buffer_;
    struct sockaddr_in udp_src_addr_;
    socklen_t udp_src_addr_len_ = sizeof(udp_src_addr_);

    RegistrarConfig config_;
    CommunicationEndpoint local_endpoint_;
    EventLoop* loop_ = nullptr;

    // Either server or client (not both)
    std::unique_ptr<RegistrarServer> server_;
    std::unique_ptr<RegistrarClient> client_;

    // Client mode registry (populated from static routes)
    std::unique_ptr<NodeRegistry> client_registry_;

    // UDP socket for sending resolution responses
    int udp_socket_ = -1;

    node_callback node_callback_;
};

// -----------------------------------------------------------------------------
// RegistrarClient - TCP client for connecting to RegistrarServer
// -----------------------------------------------------------------------------
class RegistrarClient {
public:
    RegistrarClient(const RegistrarConfig& config, CommunicationEndpoint local_endpoint,
                   CommunicationEndpoint server_endpoint, NodeRegistry* shared_registry,
                   EventLoop* loop = nullptr);
    ~RegistrarClient();

    // Non-copyable
    RegistrarClient(const RegistrarClient&) = delete;
    RegistrarClient& operator=(const RegistrarClient&) = delete;

    void start();
    void stop();

    // Set event loop for async I/O (can be changed before start())
    void set_event_loop(EventLoop* loop) { loop_ = loop; }

    // Set acceptors for registration announcement
    void set_acceptors(std::vector<AcceptorInfo> acceptors);

    // Reconnect to server (used after disconnection)
    void reconnect();

    // Check if connected
    bool is_connected() const { return connected_.load(); }

private:
    void attempt_connection();
    void send_registration();

    // Handle server messages
    void handle_server_message(TcpMessageType type, const bytes& data);
    void handle_disconnect();

    RegistrarConfig config_;
    CommunicationEndpoint local_endpoint_;
    CommunicationEndpoint server_endpoint_;
    NodeRegistry* shared_registry_;  // Not owned
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
};

} // namespace net
} // namespace hpactor