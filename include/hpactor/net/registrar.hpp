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
    void handle_accept(int client_fd);

    // Broadcast event to all connected clients
    void broadcast_node_joined(CommunicationEndpoint endpoint, const NodeEndpoint& ep);
    void broadcast_node_left(CommunicationEndpoint endpoint);

private:
    void accept_loop();
    void handle_tcp_message(int client_fd, TcpMessageType type, const bytes& data);

    // Send TCP response to client
    void send_tcp_response(int client_fd, TcpMessageType type, const bytes& payload);

    RegistrarConfig config_;
    [[maybe_unused]] CommunicationEndpoint local_endpoint_;
    NodeRegistry registry_;
    EventLoop* loop_ = nullptr;

    int tcp_socket_ = -1;
    int udp_socket_ = -1;
    std::atomic<bool> running_{false};

    // Connected clients (endpoint -> fd)
    std::unordered_map<CommunicationEndpoint, int> clients_;
    std::mutex clients_mutex_;

    std::thread accept_thread_;
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
    void failover();

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
    void connection_loop();
    void heartbeat_loop();
    void send_registration();
    void send_heartbeat();

    // TCP connection to server
    bool connect_to_server();
    void disconnect_from_server();

    // Resolve node via UDP query to server
    NodeEndpoint* resolve_node(CommunicationEndpoint endpoint);

    // Failover: try to become server or find new server
    void failover();

    // Handle connection loss (called on disconnect detection)
    void handle_connection_lost();

    // Find server via UDP broadcast
    void find_server_via_broadcast();

    RegistrarConfig config_;
    CommunicationEndpoint local_endpoint_;
    CommunicationEndpoint server_endpoint_;
    NodeRegistry* shared_registry_;  // Not owned
    EventLoop* loop_ = nullptr;

    int tcp_socket_ = -1;
    int udp_socket_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::thread connection_thread_;
    std::thread heartbeat_thread_;

    // Timer handles for EventLoop-based timers
    uint64_t connection_timer_ = 0;
    uint64_t heartbeat_timer_ = 0;

    // For heartbeat tracking
    std::chrono::steady_clock::time_point last_heartbeat_sent_;

    // Acceptors announced during registration
    std::vector<AcceptorInfo> acceptors_;
};

} // namespace net
} // namespace hpactor