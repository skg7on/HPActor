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
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <cstring>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// RegistrarConnection Implementation
// -----------------------------------------------------------------------------

RegistrarConnection::RegistrarConnection(CommunicationEndpoint remote_endpoint,
                                        EventLoop* loop,
                                        int fd)
    : remote_endpoint_(remote_endpoint),
      loop_(loop),
      fd_(fd),
      header_buffer_(TcpHeaderSize) {}

RegistrarConnection::~RegistrarConnection() {
    close();
}

RegistrarConnectionPtr RegistrarConnection::accepted(int fd,
                                                     CommunicationEndpoint remote_endpoint,
                                                     EventLoop* loop) {
    auto conn = std::shared_ptr<RegistrarConnection>(
        new RegistrarConnection(remote_endpoint, loop, fd));
    conn->register_with_loop();
    return conn;
}

RegistrarConnectionPtr RegistrarConnection::connecting(int fd,
                                                     CommunicationEndpoint remote_endpoint,
                                                     EventLoop* loop) {
    return std::shared_ptr<RegistrarConnection>(
        new RegistrarConnection(remote_endpoint, loop, fd));
}

void RegistrarConnection::register_with_loop() {
    // Note: We do NOT set loop_->set_completion_callback() here
    // because that would overwrite the server's callback.
    // Instead, the server sets ONE completion callback that routes
    // completions via a static fd->connection map.
    // This connection registers itself in that map.
    if (loop_ && fd_ >= 0) {
        loop_->add_fd(fd_, EventLoop::Event::Read);
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

void RegistrarConnection::send_message(TcpMessageType type, const bytes& payload) {
    if (fd_ < 0 || !loop_) return;

    // Build message: [Magic: 4][Version: 1][Type: 1][Length: 4][Payload: N]
    bytes message;
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
    if (fd_ < 0 || !loop_) return;

    // Non-blocking read loop (fd is known to be readable via EventLoop notification)
    // This pattern follows PlainConnection: poll has_event() to know when to read

    // Read into header buffer first
    while (header_bytes_read_ < TcpHeaderSize) {
        // Check if still readable (edge-triggered)
        if (!loop_->has_event(fd_, EventLoop::Event::Read)) {
            return;  // Would block, wait for next notification
        }

        ssize_t bytes_read = recv(fd_,
                                  header_buffer_.data() + header_bytes_read_,
                                  TcpHeaderSize - header_bytes_read_,
                                  0);
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
    if (fd_ < 0) return;

    // Continue reading payload
    while (payload_bytes_read_ < payload_buffer_.size()) {
        // Check if still readable
        if (!loop_->has_event(fd_, EventLoop::Event::Read)) {
            return;  // Would block, wait for next notification
        }

        ssize_t bytes_read = recv(fd_,
                                  payload_buffer_.data() + payload_bytes_read_,
                                  payload_buffer_.size() - payload_bytes_read_,
                                  0);
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

    // Remove sent bytes from write buffer
    if (static_cast<size_t>(result) >= write_buffer_.size()) {
        write_buffer_.clear();
    } else {
        write_buffer_.erase(write_buffer_.begin(),
                            write_buffer_.begin() + result);
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
    endpoints_[endpoint.endpoint] = endpoint;
}

bool NodeRegistry::remove_endpoint(CommunicationEndpoint endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    return endpoints_.erase(endpoint) > 0;
}

NodeEndpoint* NodeRegistry::get(CommunicationEndpoint endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = endpoints_.find(endpoint);
    if (it != endpoints_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool NodeRegistry::has(CommunicationEndpoint endpoint) const {
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

UdpRegistrar::UdpRegistrar(const RegistrarConfig& config, CommunicationEndpoint local_endpoint, EventLoop* loop)
    : config_(config),
      local_endpoint_(local_endpoint),
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
    client_registry_.reset();

    if (udp_socket_ >= 0) {
        close(udp_socket_);
        udp_socket_ = -1;
    }
}

void UdpRegistrar::start_server_mode() {
    server_ = std::make_unique<RegistrarServer>(config_, local_endpoint_, loop_);
    server_->start();

    // Create UDP socket for sending resolution responses
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
}

void UdpRegistrar::start_client_mode() {
    // Create registry populated with static routes
    client_registry_ = std::make_unique<NodeRegistry>(config_);

    // Populate with static routes
    for (const auto& route : config_.static_routes) {
        NodeEndpoint ep;
        ep.endpoint = route.endpoint;
        ep.host = route.address;
        ep.tcp_port = route.port;
        ep.is_static_route = true;
        client_registry_->upsert_endpoint(ep);
    }

    // Use first static route as server if available
    CommunicationEndpoint server_endpoint;
    if (!config_.static_routes.empty()) {
        server_endpoint = config_.static_routes[0].endpoint;
    }

    // Create UDP socket for resolution queries
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);

    client_ = std::make_unique<RegistrarClient>(config_, local_endpoint_, server_endpoint, client_registry_.get(), loop_);
    client_->start();
}

void UdpRegistrar::start_server_mode_async() {
    server_ = std::make_unique<RegistrarServer>(config_, local_endpoint_, loop_);
    server_->start();

    // Create UDP socket
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ < 0) return;

    // Bind to UDP port
    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port = htons(config_.udp_port);
    bind(udp_socket_, reinterpret_cast<struct sockaddr*>(&udp_addr), sizeof(udp_addr));

    // Register with EventLoop for read events
    if (loop_) {
        loop_->add_fd(udp_socket_, EventLoop::Event::Read);
    }

    // Allocate receive buffer
    udp_recv_buffer_.resize(kUdpRecvBufferSize);

    // Issue first async recvfrom
    issue_async_recvfrom();
}

void UdpRegistrar::start_client_mode_async() {
    // Create registry populated with static routes
    client_registry_ = std::make_unique<NodeRegistry>(config_);

    // Populate with static routes
    for (const auto& route : config_.static_routes) {
        NodeEndpoint ep;
        ep.endpoint = route.endpoint;
        ep.host = route.address;
        ep.tcp_port = route.port;
        ep.is_static_route = true;
        client_registry_->upsert_endpoint(ep);
    }

    // Use first static route as server if available
    CommunicationEndpoint server_endpoint;
    if (!config_.static_routes.empty()) {
        server_endpoint = config_.static_routes[0].endpoint;
    }

    // Create UDP socket
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ >= 0 && loop_) {
        loop_->add_fd(udp_socket_, EventLoop::Event::Read);
        udp_recv_buffer_.resize(kUdpRecvBufferSize);
        issue_async_recvfrom();
    }

    client_ = std::make_unique<RegistrarClient>(config_, local_endpoint_, server_endpoint, client_registry_.get(), loop_);
    client_->start();
}

void UdpRegistrar::issue_async_recvfrom() {
    if (udp_socket_ < 0 || !loop_) return;

    // Clear address storage for next recvfrom
    memset(&udp_src_addr_, 0, sizeof(udp_src_addr_));
    udp_src_addr_len_ = sizeof(udp_src_addr_);
}

void UdpRegistrar::handle_udp_read_ready() {
    if (udp_socket_ < 0 || !loop_) return;

    // Check if UDP socket is readable (edge-triggered)
    if (!loop_->has_event(udp_socket_, EventLoop::Event::Read)) {
        return;
    }

    // Do non-blocking recvfrom
    char buffer[kUdpRecvBufferSize];
    struct sockaddr_in src_addr;
    socklen_t src_addr_len = sizeof(src_addr);

    ssize_t bytes_read = recvfrom(udp_socket_, buffer, sizeof(buffer), 0,
                                  reinterpret_cast<struct sockaddr*>(&src_addr),
                                  &src_addr_len);

    if (bytes_read > 0) {
        bytes data(buffer, buffer + bytes_read);
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

void UdpRegistrar::handle_udp_recv_completion(const bytes& data, const std::string& from_host, uint16_t from_port) {
    // Call the existing handler
    handle_udp_packet(data, from_host, from_port);
}

void UdpRegistrar::send_udp_response(const bytes& data, const struct sockaddr_in& dest) {
    if (udp_socket_ < 0) return;

    if (loop_) {
        // Use async_sendto for async UDP send
        struct iovec iov;
        iov.iov_base = const_cast<uint8_t*>(data.data());
        iov.iov_len = data.size();

        loop_->backend()->async_sendto(
            udp_socket_,
            &iov,
            1,
            reinterpret_cast<const sockaddr*>(&dest),
            sizeof(dest),
            ActorId(0),
            static_cast<uint32_t>(OpType::SendTo));
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

    if (bind(test_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        close(test_sock);
        start_server_mode();
    } else {
        close(test_sock);
        start_client_mode();
    }
}

NodeEndpoint* UdpRegistrar::get_endpoint(CommunicationEndpoint endpoint) {
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
            // Parse protobuf payload
            PbResolveQueryPayload msg;
            if (!parse_resolve_query_payload(payload, msg)) {
                return;
            }

            CommunicationEndpoint target_endpoint = endpoint_ops::parse_endpoint(msg.target_endpoint());

            // If we have a server, look up the endpoint
            if (server_) {
                NodeEndpoint* ep = server_->registry()->get(target_endpoint);
                if (ep) {
                    // Send ResolveResponse back using protobuf
                    PbResolveResponsePayload resp_msg;
                    resp_msg.mutable_endpoint_info()->set_endpoint(endpoint_ops::to_string(ep->endpoint));
                    resp_msg.mutable_endpoint_info()->set_host(ep->host);
                    resp_msg.mutable_endpoint_info()->set_tcp_port(ep->tcp_port);

                    std::string serialized_resp = resp_msg.SerializeAsString();
                    bytes response_payload(serialized_resp.begin(), serialized_resp.end());

                    bytes response;
                    response.resize(RegistrarHeaderSize + response_payload.size());

                    uint32_t magic_be = htonl(RegistrarMagic);
                    memcpy(response.data(), &magic_be, 4);
                    response[4] = RegistrarVersion;
                    response[5] = static_cast<uint8_t>(RegistrarMessageType::ResolveResponse);
                    uint32_t len_be = htonl(static_cast<uint32_t>(response_payload.size()));
                    memcpy(response.data() + 6, &len_be, 4);
                    memcpy(response.data() + RegistrarHeaderSize, response_payload.data(), response_payload.size());

                    // Send response back to from_host:from_port via UDP
                    if (udp_socket_ >= 0) {
                        struct sockaddr_in dest_addr;
                        memset(&dest_addr, 0, sizeof(dest_addr));
                        dest_addr.sin_family = AF_INET;
                        dest_addr.sin_port = htons(from_port);
                        inet_pton(AF_INET, from_host.c_str(), &dest_addr.sin_addr);

                        sendto(udp_socket_, response.data(), response.size(), 0,
                               reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
                    }
                }
            }
            break;
        }

        case RegistrarMessageType::ResolveResponse: {
            // Parse protobuf payload
            PbResolveResponsePayload msg;
            if (!parse_resolve_response_payload(payload, msg)) {
                return;
            }

            auto& info = msg.endpoint_info();
            CommunicationEndpoint resp_endpoint = endpoint_ops::parse_endpoint(info.endpoint());
            std::string host = info.host();
            uint16_t port = static_cast<uint16_t>(info.tcp_port());

            // Add to server registry if we have one
            if (server_) {
                NodeEndpoint ep;
                ep.endpoint = resp_endpoint;
                ep.host = host;
                ep.tcp_port = port;
                ep.last_seen = std::chrono::steady_clock::now();
                server_->registry()->upsert_endpoint(ep);

                // Notify callback
                if (node_callback_) {
                    node_callback_(resp_endpoint, true);
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
