#include <hpactor/net/registrar.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <cstring>

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

UdpRegistrar::UdpRegistrar(const RegistrarConfig& config, NodeId local_node_id, EventLoop* loop)
    : config_(config),
      local_node_id_(local_node_id),
      loop_(loop) {}

UdpRegistrar::~UdpRegistrar() {
    stop();
}

void UdpRegistrar::start() {
    // Try to bind TCP port to determine if we can be server
    int test_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (test_sock < 0) {
        // Can't create socket - try client mode
        start_client_mode();
        return;
    }

    int reuse = 1;
    setsockopt(test_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.tcp_port);

    if (bind(test_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        // Success - we can be server
        close(test_sock);
        start_server_mode();
    } else {
        // Port taken - be client
        close(test_sock);
        start_client_mode();
    }
}

void UdpRegistrar::stop() {
    server_.reset();
    client_.reset();
}

void UdpRegistrar::start_server_mode() {
    server_ = std::make_unique<RegistrarServer>(config_, local_node_id_, loop_);
    server_->start();
}

void UdpRegistrar::start_client_mode() {
    // In client mode, we need to connect to a server
    // For now, use first static route as server if available
    NodeId server_node_id = 0;
    if (!config_.static_routes.empty()) {
        server_node_id = config_.static_routes[0].node_id;
    }
    client_ = std::make_unique<RegistrarClient>(config_, local_node_id_, server_node_id, nullptr, loop_);
    client_->start();
}

void UdpRegistrar::failover() {
    // Stop current mode
    server_.reset();
    client_.reset();

    // Try to become server
    int test_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (test_sock < 0) {
        start_client_mode();
        return;
    }

    int reuse = 1;
    setsockopt(test_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.tcp_port);

    if (bind(test_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        close(test_sock);
        start_server_mode();
    } else {
        close(test_sock);
        start_client_mode();
    }
}

NodeEndpoint* UdpRegistrar::get_endpoint(NodeId node_id) {
    if (server_) {
        return server_->registry()->get(node_id);
    }
    // In client mode, could query via client
    return nullptr;
}

std::vector<NodeEndpoint> UdpRegistrar::get_all_endpoints() const {
    if (server_) {
        return server_->registry()->all();
    }
    return {};
}

void UdpRegistrar::set_node_callback(node_callback cb) {
    node_callback_ = std::move(cb);
}

void UdpRegistrar::handle_udp_packet(const bytes& data, const std::string& from_host, uint16_t from_port) {
    // Handle incoming UDP packet for resolution
    // Packet format: [Magic: 4][Version: 1][Type: 1][Length: 4][Payload...]

    if (data.size() < RegistrarHeaderSize) {
        return;
    }

    // Parse header
    uint32_t magic;
    memcpy(&magic, data.data(), 4);
    magic = ntohl(magic);

    if (magic != RegistrarMagic) {
        return;
    }

    uint8_t version = data[4];
    if (version != RegistrarVersion) {
        return;
    }

    RegistrarMessageType type = static_cast<RegistrarMessageType>(data[5]);

    uint32_t payload_len;
    memcpy(&payload_len, data.data() + 6, 4);
    payload_len = ntohl(payload_len);

    if (data.size() < RegistrarHeaderSize + payload_len) {
        return;
    }

    const bytes payload(data.begin() + RegistrarHeaderSize, data.begin() + RegistrarHeaderSize + payload_len);

    switch (type) {
        case RegistrarMessageType::ResolveQuery: {
            // Query: [TargetNodeId: 4]
            if (payload.size() < 4) {
                return;
            }
            uint32_t target_id;
            memcpy(&target_id, payload.data(), 4);
            target_id = ntohl(target_id);

            // If we have a server, look up the endpoint
            if (server_) {
                NodeEndpoint* ep = server_->registry()->get(target_id);
                if (ep) {
                    // Send ResolveResponse back
                    // Response format: [Magic: 4][Version: 1][Type: 1][Length: 4][NodeId: 4][HostLen: 1][Host: N][Port: 2]
                    bytes response_payload;
                    response_payload.resize(4 + 1 + ep->host.size() + 2);

                    size_t offset = 0;
                    uint32_t node_id_be = htonl(ep->node_id);
                    memcpy(response_payload.data() + offset, &node_id_be, 4);
                    offset += 4;

                    response_payload[offset++] = static_cast<uint8_t>(ep->host.size());
                    memcpy(response_payload.data() + offset, ep->host.data(), ep->host.size());
                    offset += ep->host.size();

                    uint16_t port_be = htons(ep->tcp_port);
                    memcpy(response_payload.data() + offset, &port_be, 2);
                    offset += 2;

                    bytes response;
                    response.resize(RegistrarHeaderSize + response_payload.size());

                    uint32_t magic_be = htonl(RegistrarMagic);
                    memcpy(response.data(), &magic_be, 4);
                    response[4] = RegistrarVersion;
                    response[5] = static_cast<uint8_t>(RegistrarMessageType::ResolveResponse);
                    uint32_t len_be = htonl(static_cast<uint32_t>(response_payload.size()));
                    memcpy(response.data() + 6, &len_be, 4);
                    memcpy(response.data() + RegistrarHeaderSize, response_payload.data(), response_payload.size());

                    // Would send response back to from_host:from_port via UDP
                    (void)response;
                    (void)from_host;
                    (void)from_port;
                }
            }
            break;
        }

        case RegistrarMessageType::ResolveResponse: {
            // Response: [NodeId: 4][HostLen: 1][Host: N][Port: 2]
            if (payload.size() < 7) {
                return;
            }

            size_t offset = 0;
            uint32_t node_id;
            memcpy(&node_id, payload.data() + offset, 4);
            node_id = ntohl(node_id);
            offset += 4;

            uint8_t host_len = payload[offset++];
            if (offset + host_len + 2 > payload.size()) {
                return;
            }

            std::string host(reinterpret_cast<const char*>(payload.data() + offset), host_len);
            offset += host_len;

            uint16_t port;
            memcpy(&port, payload.data() + offset, 2);
            port = ntohs(port);

            // Add to server registry if we have one
            if (server_) {
                NodeEndpoint ep;
                ep.node_id = node_id;
                ep.host = host;
                ep.tcp_port = port;
                ep.last_seen = std::chrono::steady_clock::now();
                server_->registry()->upsert_endpoint(ep);

                // Notify callback
                if (node_callback_) {
                    node_callback_(node_id, true);
                }
            }
            break;
        }

        default:
            // Unknown message type
            break;
    }
}

} // namespace net
} // namespace hpactor
