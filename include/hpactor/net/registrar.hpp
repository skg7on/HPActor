#pragma once

#include <hpactor/types.hpp>
#include <hpactor/ref/actor_address.hpp>

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

// -----------------------------------------------------------------------------
// RegistrarConfig - configuration for UDP registrar
// -----------------------------------------------------------------------------
struct StaticRouteConfig {
    NodeId node_id = 0;
    std::string address;     // IP or DNS hostname
    uint16_t port = 0;
};

struct RegistrarConfig {
    uint16_t udp_port = 5353;                     // Default mDNS-style port
    std::chrono::milliseconds heartbeat_interval{5000};
    std::chrono::milliseconds expiration_timeout{15000};
    std::vector<StaticRouteConfig> static_routes;
    bool enable_broadcast = true;
};

// -----------------------------------------------------------------------------
// NodeEndpoint - information about a known node
// -----------------------------------------------------------------------------
struct NodeEndpoint {
    NodeId node_id = 0;
    std::string host;        // IP or DNS hostname
    uint16_t tcp_port = 0; // TCP listening port
    std::chrono::steady_clock::time_point last_seen;
    bool is_static_route = false;
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
    bool remove_endpoint(NodeId node_id);

    // Get endpoint (nullptr if not found)
    NodeEndpoint* get(NodeId node_id);

    // Check if endpoint exists
    bool has(NodeId node_id) const;

    // Get all endpoints
    std::vector<NodeEndpoint> all() const;

    // Remove expired entries
    size_t remove_expired();

private:
    RegistrarConfig config_;
    std::unordered_map<NodeId, NodeEndpoint> endpoints_;
    mutable std::mutex mutex_;
};

// -----------------------------------------------------------------------------
// UDP Registrar Protocol
// -----------------------------------------------------------------------------
enum class RegistrarMessageType : uint8_t {
    NodeAnnounce = 0x01,
    NodeQuery = 0x02,
    NodeResponse = 0x03,
    NodeLeave = 0x04,
    NodeProbe = 0x05,
    NodeProbeAck = 0x06,
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
    NodeId target_node_id;
};

struct NodeResponsePayload {
    uint16_t tcp_port;
};

struct NodeProbePayload {
    uint64_t probe_id;
    uint64_t timestamp;
};

// -----------------------------------------------------------------------------
// UdpRegistrar - UDP-based node discovery
// -----------------------------------------------------------------------------
class UdpRegistrar {
public:
    UdpRegistrar(const RegistrarConfig& config, NodeId local_node_id);
    ~UdpRegistrar();

    // Non-copyable
    UdpRegistrar(const UdpRegistrar&) = delete;
    UdpRegistrar& operator=(const UdpRegistrar&) = delete;

    // Start listening and broadcasting
    void start();
    void stop();

    // Add static route manually
    void add_static_route(NodeId node_id, const std::string& host, uint16_t port);

    // Query endpoint
    NodeEndpoint* get_endpoint(NodeId node_id);

    // Get all known endpoints
    std::vector<NodeEndpoint> get_all_endpoints() const;

    // Set callback for node online/offline events
    using node_callback = std::function<void(NodeId, bool online)>;
    void set_node_callback(node_callback cb);

    // Handle incoming UDP packet
    void handle_packet(const bytes& data, const std::string& from_host, uint16_t from_port);

private:
    // Send broadcast announcement
    void send_announce();

    // Send probe to static route
    void send_probe(NodeEndpoint& ep);

    // Parse incoming messages
    void handle_announce(NodeId sender_id, const bytes& payload,
                         const std::string& from_host, uint16_t from_port);
    void handle_query(NodeId sender_id, const bytes& payload,
                      const std::string& from_host, uint16_t from_port);
    void handle_response(NodeId sender_id, const bytes& payload,
                         const std::string& from_host, uint16_t from_port);
    void handle_leave(NodeId sender_id);
    void handle_probe(NodeId sender_id, const bytes& payload);
    void handle_probe_ack(NodeId sender_id, const bytes& payload);

    // Build outgoing messages
    bytes build_message(RegistrarMessageType type, const void* payload, size_t payload_size);

    RegistrarConfig config_;
    NodeId local_node_id_;
    NodeRegistry registry_;
    HostResolver host_resolver_;

    int udp_socket_ = -1;
    std::atomic<bool> running_{false};

    node_callback node_callback_;
    std::chrono::steady_clock::time_point last_broadcast_;

    // For probes
    std::atomic<uint64_t> next_probe_id_{1};
    std::unordered_map<uint64_t, NodeId> pending_probes_;
    std::mutex probes_mutex_;
};

} // namespace net
} // namespace hpactor