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

#include <hpactor/net/registrar.hpp>
#include <hpactor/net/registrar_serialization.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include <hpactor/log/logger.hpp>

namespace hpactor {

namespace net {

namespace {

constexpr size_t kUdpMagicOffset = 0;
constexpr size_t kUdpVersionOffset = 4;
constexpr size_t kUdpTypeOffset = 5;
constexpr size_t kUdpPayloadLengthOffset = 6;
constexpr size_t kUdpPayloadOffset = RegistrarHeaderSize;

struct UdpPacketView {
    RegistrarMessageType type = RegistrarMessageType::ResolveQuery;
    StreamBuffer payload;
};

bool parse_udp_packet(const StreamBuffer& data, UdpPacketView& packet) {
    if (data.size() < RegistrarHeaderSize) {
        return false;
    }

    uint32_t magic;
    memcpy(&magic, data.data() + kUdpMagicOffset, 4);
    magic = ntohl(magic);
    if (magic != RegistrarMagic) {
        return false;
    }

    if (data[kUdpVersionOffset] != RegistrarVersion) {
        return false;
    }

    uint32_t payload_len;
    memcpy(&payload_len, data.data() + kUdpPayloadLengthOffset, 4);
    payload_len = ntohl(payload_len);
    if (payload_len > data.size() - kUdpPayloadOffset) {
        return false;
    }

    packet.type = static_cast<RegistrarMessageType>(data[kUdpTypeOffset]);
    packet.payload.assign(data.begin() + kUdpPayloadOffset,
                          data.begin() + kUdpPayloadOffset + payload_len);
    return true;
}

StreamBuffer
make_udp_packet(RegistrarMessageType type, const StreamBuffer& payload) {
    StreamBuffer packet(RegistrarHeaderSize + payload.size());

    uint32_t magic_be = htonl(RegistrarMagic);
    memcpy(packet.data() + kUdpMagicOffset, &magic_be, 4);
    packet[kUdpVersionOffset] = RegistrarVersion;
    packet[kUdpTypeOffset] = static_cast<uint8_t>(type);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(packet.data() + kUdpPayloadLengthOffset, &len_be, 4);

    if (!payload.empty()) {
        memcpy(packet.data() + kUdpPayloadOffset, payload.data(), payload.size());
    }
    return packet;
}

void make_udp_destination(const std::string& host, uint16_t port, sockaddr_in& dest) {
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &dest.sin_addr);
}

} // namespace

// -----------------------------------------------------------------------------
// RegistrarConnection Implementation
// -----------------------------------------------------------------------------

RegistrarConnection::RegistrarConnection(EndPoint remote_endpoint,
                                         EventLoop* loop, int fd)
    : remote_endpoint_(remote_endpoint), loop_(loop), fd_(fd),
      header_buffer_(TcpHeaderSize) {}

RegistrarConnection::~RegistrarConnection() {
    close();
}

RegistrarConnectionPtr
RegistrarConnection::accepted(int fd, EndPoint remote_endpoint, EventLoop* loop) {
    auto conn = std::shared_ptr<RegistrarConnection>(
        new RegistrarConnection(remote_endpoint, loop, fd));
    conn->register_with_loop();
    return conn;
}

RegistrarConnectionPtr
RegistrarConnection::connecting(int fd, EndPoint remote_endpoint, EventLoop* loop) {
    return std::shared_ptr<RegistrarConnection>(
        new RegistrarConnection(remote_endpoint, loop, fd));
}

void RegistrarConnection::register_with_loop() {
    if (loop_ && fd_ >= 0) {
        loop_->add_fd(fd_, EventLoop::Event::Read);
        loop_->set_read_handler(fd_, [this](int /*fd*/) { handle_read_event(); });
    }
}

void RegistrarConnection::set_message_handler(message_handler h) {
    message_handler_ = std::move(h);
}

void RegistrarConnection::set_disconnect_handler(disconnect_handler h) {
    disconnect_handler_ = std::move(h);
}

void RegistrarConnection::set_send_complete_handler(send_complete_handler h) {
    send_complete_handler_ = std::move(h);
}

void RegistrarConnection::send_message(TcpMessageType type,
                                       const StreamBuffer& payload) {
    if (fd_ < 0 || !loop_)
        return;

    // Build message: [Magic: 4][Version: 1][Type: 1][Length: 4][Payload: N]
    StreamBuffer message;
    message.resize(TcpHeaderSize + payload.size());

    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = TcpRegistrarVersion;
    message[5] = static_cast<uint8_t>(type);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(message.data() + 6, &len_be, 4);
    if (!payload.empty()) {
        memcpy(message.data() + TcpHeaderSize, payload.data(), payload.size());
    }

    // Append to write buffer and flush
    write_buffer_.insert(write_buffer_.end(), message.begin(), message.end());
    flush_write_buffer();
}

void RegistrarConnection::close() {
    if (fd_ >= 0) {
        if (loop_) {
            loop_->remove_fd(fd_);
        }
        ::close(fd_);
        fd_ = -1;
    }
}

void RegistrarConnection::handle_read_event() {
    if (fd_ < 0 || !loop_)
        return;

    // Non-blocking read loop (fd is known to be readable via EventLoop
    // notification) This pattern follows PlainConnection: poll has_event() to
    // know when to read

    // Read into header buffer first
    while (header_bytes_read_ < TcpHeaderSize) {
        // Check if still readable (edge-triggered)
        if (!loop_->has_event(fd_, EventLoop::Event::Read)) {
            return; // Would block, wait for next notification
        }

        ssize_t bytes_read = recv(fd_, header_buffer_.data() + header_bytes_read_,
                                  TcpHeaderSize - header_bytes_read_, 0);
        if (bytes_read <= 0) {
            // Connection closed or error
            if (disconnect_handler_) {
                disconnect_handler_();
            }
            close();
            return;
        }
        header_bytes_read_ += static_cast<size_t>(bytes_read);
    }

    // Parse header
    uint32_t magic;
    memcpy(&magic, header_buffer_.data(), 4);
    magic = ntohl(magic);

    if (magic != TcpRegistrarMagic) {
        // Invalid magic - consume byte and try again
        memmove(header_buffer_.data(), header_buffer_.data() + 1, TcpHeaderSize - 1);
        header_bytes_read_--;
        // Loop back to try reading more header bytes
        return;
    }

    uint8_t version = header_buffer_[4];
    if (version != TcpRegistrarVersion) {
        // Unsupported version - consume byte and try again
        memmove(header_buffer_.data(), header_buffer_.data() + 1, TcpHeaderSize - 1);
        header_bytes_read_--;
        return;
    }

    current_type_ = static_cast<TcpMessageType>(header_buffer_[5]);

    uint32_t payload_len;
    memcpy(&payload_len, header_buffer_.data() + 6, 4);
    payload_len = ntohl(payload_len);

    // Allocate payload buffer
    payload_buffer_.resize(payload_len);
    payload_bytes_read_ = 0;
    read_state_ = ReadState::ReadingPayload;

    // Continue to read payload
    handle_payload_read();
}

void RegistrarConnection::handle_payload_read() {
    if (fd_ < 0)
        return;

    // Continue reading payload
    while (payload_bytes_read_ < payload_buffer_.size()) {
        // Check if still readable
        if (!loop_->has_event(fd_, EventLoop::Event::Read)) {
            return; // Would block, wait for next notification
        }

        ssize_t bytes_read =
            recv(fd_, payload_buffer_.data() + payload_bytes_read_,
                 payload_buffer_.size() - payload_bytes_read_, 0);
        if (bytes_read <= 0) {
            // Connection closed or error
            if (disconnect_handler_) {
                disconnect_handler_();
            }
            close();
            return;
        }
        payload_bytes_read_ += static_cast<size_t>(bytes_read);
    }

    // Payload complete - deliver to handler
    if (message_handler_) {
        message_handler_(current_type_, payload_buffer_);
    }

    // Reset to reading next header
    header_bytes_read_ = 0;
    std::fill(header_buffer_.begin(), header_buffer_.end(), 0);
    payload_buffer_.clear();
    read_state_ = ReadState::ReadingHeader;
}

void RegistrarConnection::flush_write_buffer() {
    if (fd_ < 0 || loop_ == nullptr || write_buffer_.empty() || is_sending_) {
        return;
    }

    is_sending_ = true;

    struct iovec iov;
    iov.iov_base = write_buffer_.data();
    iov.iov_len = write_buffer_.size();

    // Use async_send - completion routed via EventLoop completion_callback_
    loop_->backend()->async_send(fd_, &iov, 1, ActorId(0),
                                 static_cast<uint32_t>(OpType::Send));
}

void RegistrarConnection::handle_send_completion(int result) {
    if (send_complete_handler_) {
        send_complete_handler_(result);
    }
    is_sending_ = false;

    if (result < 0) {
        // Send error
        if (disconnect_handler_) {
            disconnect_handler_();
        }
        close();
        return;
    }

    // Remove sent StreamBuffer from write buffer
    if (static_cast<size_t>(result) >= write_buffer_.size()) {
        write_buffer_.clear();
    } else {
        write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + result);
    }

    // Continue flushing if more data
    if (!write_buffer_.empty()) {
        flush_write_buffer();
    }
}

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
        struct sockaddr_in* addr_in =
            reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
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
    for (auto it = cache_.begin(); it != cache_.end();) {
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

NodeRegistry::NodeRegistry(const RegistrarConfig& config) : config_(config) {}

void NodeRegistry::upsert_endpoint(NodeEndpoint endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    endpoint.last_seen = std::chrono::steady_clock::now();
    endpoints_[endpoint.identity.endpoint] = endpoint;
}

bool NodeRegistry::remove_endpoint(EndPoint endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    return endpoints_.erase(endpoint) > 0;
}

NodeEndpoint* NodeRegistry::get(EndPoint endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = endpoints_.find(endpoint);
    if (it != endpoints_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool NodeRegistry::has(EndPoint endpoint) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return endpoints_.find(endpoint) != endpoints_.end();
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
    for (auto it = endpoints_.begin(); it != endpoints_.end();) {
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

UdpRegistrar::UdpRegistrar(const RegistrarConfig& config,
                           EndPoint local_endpoint, EventLoop* loop)
    : config_(config), local_endpoint_(local_endpoint), loop_(loop) {}

UdpRegistrar::~UdpRegistrar() {
    stop();
}

void UdpRegistrar::start() {
    if (loop_) {
        // Event-driven path — use the EventLoop-integrated async methods.
        // Probe server vs client mode by attempting to bind the TCP port.
        int test_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (test_sock < 0) {
            start_client_mode_async();
            return;
        }

        int reuse = 1;
        setsockopt(test_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(config_.tcp_port);

        if (bind(test_sock, reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) == 0) {
            close(test_sock);
            start_server_mode_async();
        } else {
            close(test_sock);
            start_client_mode_async();
        }
        return;
    }

    // Legacy path — no EventLoop available, use blocking socket I/O.
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

    if (bind(test_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) ==
        0) {
        close(test_sock);
        start_server_mode();
    } else {
        close(test_sock);
        start_client_mode();
    }
}

void UdpRegistrar::stop() {
    server_.reset();
    client_.reset();
    client_registry_.reset();

    if (udp_socket_ >= 0) {
        if (loop_) {
            loop_->clear_read_handler(udp_socket_);
            loop_->remove_fd(udp_socket_);
        }
        close(udp_socket_);
        udp_socket_ = -1;
    }
}

void UdpRegistrar::start_server_mode() {
    server_ = std::make_unique<RegistrarServer>(config_, local_endpoint_, loop_);
    server_->start();

    setup_udp_socket();
}

void UdpRegistrar::start_client_mode() {
    // Create registry populated with static routes
    client_registry_ = std::make_unique<NodeRegistry>(config_);

    // Populate with static routes
    for (const auto& route : config_.static_routes) {
        NodeEndpoint ep;
        ep.identity.endpoint = route.endpoint;
        ep.identity.host = route.address;
        ep.tcp_port = route.port;
        ep.is_static_route = true;
        client_registry_->upsert_endpoint(ep);
    }

    // Use first static route as server if available
    EndPoint server_endpoint;
    if (!config_.static_routes.empty()) {
        server_endpoint = config_.static_routes[0].endpoint;
    }

    client_ = std::make_unique<RegistrarClient>(
        config_, local_endpoint_, server_endpoint, client_registry_.get(), loop_);
    client_->set_failover_callback([this]() { failover(); });
    client_->start();
}

void UdpRegistrar::setup_udp_socket() {
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ < 0)
        return;

    int reuse = 1;
    setsockopt(udp_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port = htons(config_.udp_port);
    if (bind(udp_socket_, reinterpret_cast<struct sockaddr*>(&udp_addr),
             sizeof(udp_addr)) < 0) {
        close(udp_socket_);
        udp_socket_ = -1;
        return;
    }

    if (loop_) {
        loop_->add_fd(udp_socket_, EventLoop::Event::Read);
        loop_->set_read_handler(udp_socket_,
                                [this](int /*fd*/) { handle_udp_read_ready(); });
    }
}

void UdpRegistrar::start_server_mode_async() {
    server_ = std::make_unique<RegistrarServer>(config_, local_endpoint_, loop_);
    server_->start();

    setup_udp_socket();
    if (udp_socket_ < 0)
        return;

    udp_recv_buffer_.resize(kUdpRecvBufferSize);
    issue_async_recvfrom();
}

void UdpRegistrar::start_client_mode_async() {
    // Create registry populated with static routes
    client_registry_ = std::make_unique<NodeRegistry>(config_);

    // Populate with static routes
    for (const auto& route : config_.static_routes) {
        NodeEndpoint ep;
        ep.identity.endpoint = route.endpoint;
        ep.identity.host = route.address;
        ep.tcp_port = route.port;
        ep.is_static_route = true;
        client_registry_->upsert_endpoint(ep);
    }

    // Use first static route as server if available
    EndPoint server_endpoint;
    if (!config_.static_routes.empty()) {
        server_endpoint = config_.static_routes[0].endpoint;
    }

    client_ = std::make_unique<RegistrarClient>(
        config_, local_endpoint_, server_endpoint, client_registry_.get(), loop_);
    client_->set_failover_callback([this]() { failover(); });
    client_->start();
}

void UdpRegistrar::issue_async_recvfrom() {
    if (udp_socket_ < 0 || !loop_)
        return;

    // Clear address storage for next recvfrom
    memset(&udp_src_addr_, 0, sizeof(udp_src_addr_));
    udp_src_addr_len_ = sizeof(udp_src_addr_);
}

void UdpRegistrar::handle_udp_read_ready() {
    if (udp_socket_ < 0)
        return;

    // Non-blocking recvfrom — the EventLoop only calls this callback
    // when the fd is readable (edge-triggered epoll/kqueue).
    char buffer[kUdpRecvBufferSize];
    struct sockaddr_in src_addr;
    socklen_t src_addr_len = sizeof(src_addr);

    ssize_t bytes_read =
        recvfrom(udp_socket_, buffer, sizeof(buffer), 0,
                 reinterpret_cast<struct sockaddr*>(&src_addr), &src_addr_len);

    if (bytes_read > 0) {
        StreamBuffer data(buffer, buffer + bytes_read);
        char ip_str[INET_ADDRSTRLEN];
        std::string from_host;
        uint16_t from_port = 0;

        if (inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str))) {
            from_host = ip_str;
        }
        from_port = ntohs(src_addr.sin_port);

        handle_udp_recv_completion(data, from_host, from_port);
    }
}

void UdpRegistrar::handle_udp_recv_completion(const StreamBuffer& data,
                                              const std::string& from_host,
                                              uint16_t from_port) {
    // Call the existing handler
    handle_udp_packet(data, from_host, from_port);
}

void UdpRegistrar::send_udp_response(const StreamBuffer& data,
                                     const struct sockaddr_in& dest) {
    if (udp_socket_ < 0)
        return;

    if (loop_) {
        // Use async_sendto for async UDP send
        struct iovec iov;
        iov.iov_base = const_cast<uint8_t*>(data.data());
        iov.iov_len = data.size();

        loop_->backend()->async_sendto(
            udp_socket_, &iov, 1, reinterpret_cast<const sockaddr*>(&dest),
            sizeof(dest), ActorId(0), static_cast<uint32_t>(OpType::SendTo));
    } else {
        // Fallback to blocking sendto
        sendto(udp_socket_, data.data(), data.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&dest), sizeof(dest));
    }
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

    if (bind(test_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) ==
        0) {
        close(test_sock);
        start_server_mode();
    } else {
        close(test_sock);
        start_client_mode();
    }
}

NodeEndpoint* UdpRegistrar::get_endpoint(EndPoint endpoint) {
    if (server_) {
        return server_->registry()->get(endpoint);
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

void UdpRegistrar::handle_udp_packet(const StreamBuffer& data,
                                     const std::string& from_host,
                                     uint16_t from_port) {
    // Handle incoming UDP packet for resolution
    // Packet format: [Magic: 4][Version: 1][Type: 1][Length: 4]
    // [Reserved: 2][Payload...]
    UdpPacketView packet;
    if (!parse_udp_packet(data, packet)) {
        HPACTOR_LOG_ERROR(log::LogCategory::kRegistrar, ActorId{0}, 0,
                          "malformed registrar packet");
        return;
    }

    switch (packet.type) {
        case RegistrarMessageType::ResolveQuery:
            handle_resolve_query(packet.payload, from_host, from_port);
            break;

        case RegistrarMessageType::ResolveResponse:
            handle_resolve_response(packet.payload);
            break;

        default:
            // Unknown message type
            break;
    }
}

void UdpRegistrar::handle_resolve_query(const StreamBuffer& payload,
                                        const std::string& from_host,
                                        uint16_t from_port) {
    if (!server_) {
        return;
    }

    PbResolveQueryPayload msg;
    if (!parse_resolve_query_payload(payload, msg)) {
        return;
    }

    EndPoint target_endpoint = endpoint_ops::parse_endpoint(msg.target_endpoint());
    NodeEndpoint* ep = server_->registry()->get(target_endpoint);
    if (ep == nullptr) {
        HPACTOR_LOG_WARNING(
            log::LogCategory::kRegistrar, ActorId{0},
            static_cast<uint32_t>(log::LogEventId::kRegistrarResolveMiss),
            "registrar resolve miss");
        return;
    }

    send_resolve_response(*ep, from_host, from_port);
}

void UdpRegistrar::handle_resolve_response(const StreamBuffer& payload) {
    PbResolveResponsePayload msg;
    if (!parse_resolve_response_payload(payload, msg)) {
        return;
    }

    if (!server_) {
        return;
    }

    auto& info = msg.endpoint_info();
    NodeEndpoint ep;
    ep.identity.endpoint = endpoint_ops::parse_endpoint(info.endpoint());
    ep.identity.host = info.host();
    ep.tcp_port = static_cast<uint16_t>(info.tcp_port());
    ep.last_seen = std::chrono::steady_clock::now();
    server_->registry()->upsert_endpoint(ep);

    if (node_callback_) {
        node_callback_(ep.identity.endpoint, true);
    }
}

void UdpRegistrar::send_resolve_response(const NodeEndpoint& endpoint,
                                         const std::string& from_host,
                                         uint16_t from_port) const {
    if (udp_socket_ < 0) {
        return;
    }

    sockaddr_in dest_addr;
    make_udp_destination(from_host, from_port, dest_addr);

    StreamBuffer response_payload = serialize_resolve_response_payload(endpoint);
    StreamBuffer response =
        make_udp_packet(RegistrarMessageType::ResolveResponse, response_payload);
    sendto(udp_socket_, response.data(), response.size(), 0,
           reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
}

// ── IServiceDiscovery overrides ────────────────────────────────────────────

Member UdpRegistrar::to_member(const NodeEndpoint& ep) {
    Member m;
    m.identity.endpoint = ep.identity.endpoint;
    m.identity.host = ep.identity.host;
    m.identity.uds_path = ep.identity.uds_path;
    m.identity.acceptors = ep.identity.acceptors;
    m.last_seen = ep.last_seen;
    return m;
}

std::vector<Member> UdpRegistrar::discover_all() const {
    std::vector<Member> result;

    // Collect from server registry
    auto eps = get_all_endpoints();
    result.reserve(eps.size());
    for (const auto& ep : eps)
        result.push_back(to_member(ep));

    // Also collect from client registry (static routes)
    if (client_registry_) {
        auto client_eps = client_registry_->all();
        result.reserve(result.size() + client_eps.size());
        for (const auto& ep : client_eps)
            result.push_back(to_member(ep));
    }

    return result;
}

const Member* UdpRegistrar::discover(EndPoint ep) const {
    // Check server registry first
    if (server_) {
        auto* node_ep = server_->registry()->get(ep);
        if (node_ep) {
            endpoint_to_member_[ep] = to_member(*node_ep);
            return &endpoint_to_member_[ep];
        }
    }

    // Check client registry
    if (client_registry_) {
        auto* node_ep = client_registry_->get(ep);
        if (node_ep) {
            endpoint_to_member_[ep] = to_member(*node_ep);
            return &endpoint_to_member_[ep];
        }
    }

    return nullptr;
}

void UdpRegistrar::announce(Member) {
    // No-op: registrar server handles membership via Register/Heartbeat.
}

void UdpRegistrar::on_member_change(MemberChangeCallback cb) {
    member_change_cb_ = std::move(cb);
}

} // namespace net
} // namespace hpactor
