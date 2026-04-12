#include <hpactor/net/registrar.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace {

// Byte-swap a 64-bit value (big-endian to host-endian)
inline uint64_t swap_be64(uint64_t v) {
    return ((v & 0x00000000000000FFULL) << 56) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x00000000FF000000ULL) <<  8) |
           ((v & 0x000000FF00000000ULL) >>  8) |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0xFF00000000000000ULL) >> 56);
}

} // anonymous namespace

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

    // Enable broadcast
    int broadcast = 1;
    if (setsockopt(udp_socket_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        close(udp_socket_);
        udp_socket_ = -1;
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

    // Send initial broadcast
    send_announce();
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

void UdpRegistrar::handle_packet(const bytes& data, const std::string& from_host, uint16_t from_port) {
    if (data.size() < RegistrarHeaderSize) {
        return;
    }

    // Parse header
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    NodeId sender_id;

    std::memcpy(&magic, data.data(), sizeof(magic));
    magic = ntohl(magic);

    if (magic != RegistrarMagic) {
        return;
    }

    version = data[4];
    if (version != RegistrarVersion) {
        return;
    }

    type = data[5];
    std::memcpy(&sender_id, data.data() + 6, sizeof(sender_id));
    sender_id = ntohl(sender_id);

    // Ignore messages from self
    if (sender_id == local_node_id_) {
        return;
    }

    const bytes payload(data.begin() + RegistrarHeaderSize, data.end());

    switch (static_cast<RegistrarMessageType>(type)) {
        case RegistrarMessageType::NodeJoin:
            handle_announce(sender_id, payload, from_host, from_port);
            break;
        case RegistrarMessageType::ResolveQuery:
            handle_query(sender_id, payload, from_host, from_port);
            break;
        case RegistrarMessageType::ResolveResponse:
            handle_response(sender_id, payload, from_host, from_port);
            break;
        case RegistrarMessageType::NodeLeave:
            handle_leave(sender_id);
            break;
        case RegistrarMessageType::Register:
            handle_probe(sender_id, payload);
            break;
        case RegistrarMessageType::Heartbeat:
            handle_probe_ack(sender_id, payload);
            break;
        case RegistrarMessageType::Accept:
        case RegistrarMessageType::Error:
            // TCP messages - not handled in UDP registrar
            break;
    }
}

void UdpRegistrar::send_announce() {
    if (udp_socket_ < 0 || config_.disable_server) {
        return;
    }

    NodeAnnouncePayload payload;
    payload.tcp_port = 0;  // Would be set from actual TCP port
    payload.actor_count = 0;

    bytes msg = build_message(RegistrarMessageType::NodeJoin, &payload, sizeof(payload));

    // Broadcast to 255.255.255.255
    struct sockaddr_in dest_addr;
    std::memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(config_.udp_port);
    dest_addr.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(udp_socket_, msg.data(), msg.size(), 0,
           reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
}

void UdpRegistrar::send_probe(NodeEndpoint& ep) {
    if (udp_socket_ < 0) {
        return;
    }

    uint64_t probe_id = next_probe_id_++;

    // Store pending probe
    {
        std::lock_guard<std::mutex> lock(probes_mutex_);
        pending_probes_[probe_id] = ep.node_id;
    }

    NodeProbePayload payload;
    payload.probe_id = probe_id;
    payload.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    bytes msg = build_message(RegistrarMessageType::Register, &payload, sizeof(payload));

    // Resolve hostname
    std::string ip = host_resolver_.resolve(ep.host);
    if (ip.empty()) {
        return;
    }

    struct sockaddr_in dest_addr;
    std::memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(config_.udp_port);
    inet_pton(AF_INET, ip.c_str(), &dest_addr.sin_addr);

    sendto(udp_socket_, msg.data(), msg.size(), 0,
           reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
}

void UdpRegistrar::handle_announce(NodeId sender_id, const bytes& payload,
                                    const std::string& from_host, uint16_t /*from_port*/) {
    if (payload.size() < sizeof(NodeAnnouncePayload)) {
        return;
    }

    NodeAnnouncePayload announce;
    std::memcpy(&announce, payload.data(), sizeof(announce));
    announce.tcp_port = ntohs(announce.tcp_port);
    announce.actor_count = ntohs(announce.actor_count);

    NodeEndpoint ep;
    ep.node_id = sender_id;
    ep.host = from_host;
    ep.tcp_port = announce.tcp_port;
    ep.is_static_route = false;
    ep.last_seen = std::chrono::steady_clock::now();

    bool is_new = !registry_.has(sender_id);
    registry_.upsert_endpoint(ep);

    if (node_callback_ && is_new) {
        node_callback_(sender_id, true);
    }
}

void UdpRegistrar::handle_query(NodeId /*sender_id*/, const bytes& payload,
                                 const std::string& from_host, uint16_t from_port) {
    if (payload.size() < sizeof(NodeQueryPayload)) {
        return;
    }

    NodeQueryPayload query;
    std::memcpy(&query, payload.data(), sizeof(query));
    query.target_node_id = ntohl(query.target_node_id);

    // If we have the target, send response
    if (query.target_node_id == local_node_id_) {
        // This query was for us
        NodeResponsePayload resp;
        resp.tcp_port = 0;  // Would be our TCP port
        bytes msg = build_message(RegistrarMessageType::ResolveResponse, &resp, sizeof(resp));

        struct sockaddr_in dest_addr;
        std::memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(from_port);
        inet_pton(AF_INET, from_host.c_str(), &dest_addr.sin_addr);

        sendto(udp_socket_, msg.data(), msg.size(), 0,
               reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
    }
}

void UdpRegistrar::handle_response(NodeId sender_id, const bytes& payload,
                                    const std::string& from_host, uint16_t /*from_port*/) {
    if (payload.size() < sizeof(NodeResponsePayload)) {
        return;
    }

    NodeResponsePayload resp;
    std::memcpy(&resp, payload.data(), sizeof(resp));
    resp.tcp_port = ntohs(resp.tcp_port);

    NodeEndpoint* ep = registry_.get(sender_id);
    if (ep != nullptr) {
        ep->host = from_host;
        ep->tcp_port = resp.tcp_port;
        ep->last_seen = std::chrono::steady_clock::now();
    }
}

void UdpRegistrar::handle_leave(NodeId sender_id) {
    if (registry_.remove_endpoint(sender_id)) {
        if (node_callback_) {
            node_callback_(sender_id, false);
        }
    }
}

void UdpRegistrar::handle_probe(NodeId sender_id, const bytes& payload) {
    if (payload.size() < sizeof(NodeProbePayload)) {
        return;
    }

    NodeProbePayload probe;
    std::memcpy(&probe, payload.data(), sizeof(probe));
    probe.probe_id = swap_be64(probe.probe_id);
    probe.timestamp = swap_be64(probe.timestamp);

    // Send ack
    NodeProbePayload ack;
    ack.probe_id = probe.probe_id;
    ack.timestamp = probe.timestamp;

    bytes msg = build_message(RegistrarMessageType::Heartbeat, &ack, sizeof(ack));

    NodeEndpoint* ep = registry_.get(sender_id);
    if (ep == nullptr) {
        return;
    }

    std::string ip = host_resolver_.resolve(ep->host);
    if (ip.empty()) {
        return;
    }

    struct sockaddr_in dest_addr;
    std::memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(config_.udp_port);
    inet_pton(AF_INET, ip.c_str(), &dest_addr.sin_addr);

    sendto(udp_socket_, msg.data(), msg.size(), 0,
           reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
}

void UdpRegistrar::handle_probe_ack(NodeId /*sender_id*/, const bytes& payload) {
    if (payload.size() < sizeof(NodeProbePayload)) {
        return;
    }

    NodeProbePayload ack;
    std::memcpy(&ack, payload.data(), sizeof(ack));
    ack.probe_id = swap_be64(ack.probe_id);
    ack.timestamp = swap_be64(ack.timestamp);

    // Remove from pending probes
    NodeId probed_node_id = 0;
    {
        std::lock_guard<std::mutex> lock(probes_mutex_);
        auto it = pending_probes_.find(ack.probe_id);
        if (it != pending_probes_.end()) {
            probed_node_id = it->second;
            pending_probes_.erase(it);
        }
    }

    // Update endpoint
    if (probed_node_id != 0) {
        NodeEndpoint* ep = registry_.get(probed_node_id);
        if (ep != nullptr) {
            ep->last_seen = std::chrono::steady_clock::now();
        }
    }
}

bytes UdpRegistrar::build_message(RegistrarMessageType type, const void* payload, size_t payload_size) {
    bytes msg;
    msg.reserve(RegistrarHeaderSize + payload_size);

    // Magic (4 bytes, big-endian)
    uint32_t magic = htonl(RegistrarMagic);
    msg.insert(msg.end(), reinterpret_cast<const uint8_t*>(&magic),
               reinterpret_cast<const uint8_t*>(&magic) + sizeof(magic));

    // Version (1 byte)
    msg.push_back(RegistrarVersion);

    // Message type (1 byte)
    msg.push_back(static_cast<uint8_t>(type));

    // Node ID (4 bytes, big-endian)
    uint32_t node_id_be = htonl(local_node_id_);
    msg.insert(msg.end(), reinterpret_cast<const uint8_t*>(&node_id_be),
               reinterpret_cast<const uint8_t*>(&node_id_be) + sizeof(node_id_be));

    // Payload
    if (payload != nullptr && payload_size > 0) {
        msg.insert(msg.end(), reinterpret_cast<const uint8_t*>(payload),
                   reinterpret_cast<const uint8_t*>(payload) + payload_size);
    }

    return msg;
}

} // namespace net
} // namespace hpactor