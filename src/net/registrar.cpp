#include <hpactor/net/registrar.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

// swap_be64 will be needed in Task 5 TCP implementation
// Defined there when needed

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// HostResolver Implementation
// -----------------------------------------------------------------------------

std::string HostResolver::resolve(const std::string& hostname) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check cache first
    auto it = cache_.find(hostname);
    if (it != cache_.end()) {
        if (it->second.expires_at > std::chrono::steady_clock::now()) {
            return it->second.ip;
        }
        // Expired - remove it
        cache_.erase(it);
    }

    // Check if hostname is already an IP address
    struct in_addr addr;
    if (inet_pton(AF_INET, hostname.c_str(), &addr) == 1) {
        // It's a valid IP address, cache it
        cache(hostname, hostname, std::chrono::seconds(300));
        return hostname;
    }

    // Try DNS resolution
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int ret = getaddrinfo(hostname.c_str(), nullptr, &hints, &result);
    if (ret != 0) {
        return "";
    }

    std::string ip;
    if (result != nullptr) {
        char ipstr[INET_ADDRSTRLEN];
        struct sockaddr_in* addr_in = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
        if (inet_ntop(AF_INET, &addr_in->sin_addr, ipstr, sizeof(ipstr)) != nullptr) {
            ip = ipstr;
        }
        freeaddrinfo(result);
    }

    if (!ip.empty()) {
        cache(hostname, ip, std::chrono::seconds(300));
    }

    return ip;
}

void HostResolver::resolve_async(const std::string& hostname,
                                  std::function<void(std::string ip)> callback) {
    // For now, do blocking resolution in a background context
    // In production, this would use a thread pool
    std::string result = resolve(hostname);
    callback(result);
}

std::string HostResolver::get_cached(const std::string& hostname) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(hostname);
    if (it != cache_.end()) {
        if (it->second.expires_at > std::chrono::steady_clock::now()) {
            return it->second.ip;
        }
    }
    return "";
}

void HostResolver::cache(const std::string& hostname, const std::string& ip,
                          std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheEntry entry;
    entry.ip = ip;
    entry.expires_at = std::chrono::steady_clock::now() + ttl;
    cache_[hostname] = entry;
}

void HostResolver::clear_expired() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache_.begin(); it != cache_.end(); ) {
        if (it->second.expires_at <= now) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

// -----------------------------------------------------------------------------
// NodeRegistry Implementation
// -----------------------------------------------------------------------------

NodeRegistry::NodeRegistry(const RegistrarConfig& config)
    : config_(config) {}

void NodeRegistry::upsert_endpoint(NodeEndpoint endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    endpoint.last_seen = std::chrono::steady_clock::now();
    endpoints_[endpoint.node_id] = endpoint;
}

bool NodeRegistry::remove_endpoint(NodeId node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return endpoints_.erase(node_id) > 0;
}

NodeEndpoint* NodeRegistry::get(NodeId node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = endpoints_.find(node_id);
    if (it != endpoints_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool NodeRegistry::has(NodeId node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return endpoints_.find(node_id) != endpoints_.end();
}

std::vector<NodeEndpoint> NodeRegistry::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NodeEndpoint> result;
    result.reserve(endpoints_.size());
    for (const auto& [id, ep] : endpoints_) {
        result.push_back(ep);
    }
    return result;
}

size_t NodeRegistry::remove_expired() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    size_t removed = 0;
    for (auto it = endpoints_.begin(); it != endpoints_.end(); ) {
        // Static routes don't expire
        if (!it->second.is_static_route) {
            auto age = now - it->second.last_seen;
            if (age > config_.expiration_timeout) {
                it = endpoints_.erase(it);
                ++removed;
                continue;
            }
        }
        ++it;
    }
    return removed;
}

// -----------------------------------------------------------------------------
// UdpRegistrar Implementation
// -----------------------------------------------------------------------------
// NOTE: This is a stub implementation. The full TCP server/client implementation
// will be added in Tasks 3-5. For now, this provides minimal functionality
// to allow compilation with the updated types.

UdpRegistrar::UdpRegistrar(const RegistrarConfig& config, NodeId local_node_id)
    : config_(config),
      local_node_id_(local_node_id),
      registry_(config),
      last_broadcast_(std::chrono::steady_clock::now()) {}

UdpRegistrar::~UdpRegistrar() {
    stop();
}

void UdpRegistrar::start() {
    if (running_.load()) {
        return;
    }

    // Create UDP socket
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ < 0) {
        return;
    }

    // Bind to UDP port
    struct sockaddr_in bind_addr;
    std::memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(config_.udp_port);

    if (bind(udp_socket_, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        close(udp_socket_);
        udp_socket_ = -1;
        return;
    }

    running_.store(true);

    // Add static routes to registry
    for (const auto& route : config_.static_routes) {
        NodeEndpoint ep;
        ep.node_id = route.node_id;
        ep.host = route.address;
        ep.tcp_port = route.port;
        ep.is_static_route = true;
        ep.last_seen = std::chrono::steady_clock::now();
        registry_.upsert_endpoint(ep);
    }
}

void UdpRegistrar::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    if (udp_socket_ >= 0) {
        close(udp_socket_);
        udp_socket_ = -1;
    }
}

void UdpRegistrar::add_static_route(NodeId node_id, const std::string& host, uint16_t port) {
    NodeEndpoint ep;
    ep.node_id = node_id;
    ep.host = host;
    ep.tcp_port = port;
    ep.is_static_route = true;
    ep.last_seen = std::chrono::steady_clock::now();
    registry_.upsert_endpoint(ep);
}

NodeEndpoint* UdpRegistrar::get_endpoint(NodeId node_id) {
    return registry_.get(node_id);
}

std::vector<NodeEndpoint> UdpRegistrar::get_all_endpoints() const {
    return registry_.all();
}

void UdpRegistrar::set_node_callback(node_callback cb) {
    node_callback_ = std::move(cb);
}

void UdpRegistrar::handle_packet(const bytes& /*data*/, const std::string& /*from_host*/, uint16_t /*from_port*/) {
    // Will be implemented in Task 5 with TCP server/client
}

bytes UdpRegistrar::build_message(RegistrarMessageType /*type*/, const void* /*payload*/, size_t /*payload_size*/) {
    // Will be implemented properly in Task 5
    return bytes();
}

} // namespace net
} // namespace hpactor