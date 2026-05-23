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

#include <hpactor/net/acceptor.hpp>

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// Acceptor (abstract base)
// -----------------------------------------------------------------------------

Acceptor::Acceptor(EventLoop* loop) : loop_(loop), listening_fd_(-1) {}

Acceptor::~Acceptor() {
    close();
}

void Acceptor::close() {
    if (listening_fd_ >= 0) {
        if (loop_ != nullptr) {
            loop_->clear_read_handler(listening_fd_);
            loop_->remove_fd(listening_fd_);
        }
        ::close(listening_fd_);
        listening_fd_ = -1;
    }
}

void Acceptor::set_accept_handler(accept_handler handler) {
    accept_handler_ = std::move(handler);
}

// -----------------------------------------------------------------------------
// TcpAcceptor
// -----------------------------------------------------------------------------

bool TcpAcceptor::listen(uint16_t port, uint16_t port_range,
                         const std::string& bind_address) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    // Set SO_REUSEADDR
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (bind_address == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr);
    }
    addr.sin_port = htons(port);

    // Try binding, with port range fallback
    bool bound = false;
    uint16_t const end_port = static_cast<uint16_t>(port + port_range);
    for (uint16_t p = port; p <= end_port; ++p) {
        addr.sin_port = htons(p);
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            bound = true;
            bound_port_ = p;
            break;
        }
    }

    if (!bound) {
        ::close(fd);
        return false;
    }

    // Listen
    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        return false;
    }

    listening_fd_ = fd;

    // Register with event loop
    if (loop_ != nullptr) {
        loop_->add_fd(listening_fd_, EventLoop::Event::Read);
        loop_->set_read_handler(listening_fd_, [this](int) { handle_read(); });
    }

    return true;
}

void TcpAcceptor::handle_read() {
    struct sockaddr_storage client_addr{};
    socklen_t client_len = sizeof(client_addr);

    int client_fd =
        accept(listening_fd_, reinterpret_cast<struct sockaddr*>(&client_addr),
               &client_len);

    if (client_fd < 0) {
        return;
    }

    // Set TCP_NODELAY for low latency
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Set non-blocking
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    if (accept_handler_) {
        EndPoint endpoint;
        if (client_addr.ss_family == AF_INET) {
            auto* in4 = reinterpret_cast<struct sockaddr_in*>(&client_addr);
            endpoint = Ipv4Endpoint{in4->sin_addr.s_addr, in4->sin_port};
        } else if (client_addr.ss_family == AF_INET6) {
            auto* in6 = reinterpret_cast<struct sockaddr_in6*>(&client_addr);
            std::array<uint8_t, 16> addr{};
            std::memcpy(addr.data(), &in6->sin6_addr, 16);
            endpoint = Ipv6Endpoint{addr, in6->sin6_port};
        } else {
            ::close(client_fd);
            return;
        }
        accept_handler_(client_fd, endpoint);
    } else {
        ::close(client_fd);
    }
}

// -----------------------------------------------------------------------------
// UnixDomainAcceptor
// -----------------------------------------------------------------------------

bool UnixDomainAcceptor::listen(const std::string& path) {
    // Remove stale socket file if it exists
    ::unlink(path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        return false;
    }

    // Only set after successful listen to avoid fd leak on failure
    listening_fd_ = fd;
    uds_path_ = path;

    if (loop_ != nullptr) {
        loop_->add_fd(listening_fd_, EventLoop::Event::Read);
        loop_->set_read_handler(listening_fd_, [this](int) { handle_read(); });
    }

    return true;
}

void UnixDomainAcceptor::close() {
    if (!uds_path_.empty()) {
        ::unlink(uds_path_.c_str());
        uds_path_.clear();
    }
    Acceptor::close();
}

void UnixDomainAcceptor::handle_read() {
    struct sockaddr_un client_addr{};
    socklen_t client_len = sizeof(client_addr);

    int client_fd =
        accept(listening_fd_, reinterpret_cast<struct sockaddr*>(&client_addr),
               &client_len);

    if (client_fd < 0) {
        return;
    }

    // Set non-blocking
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    if (accept_handler_) {
        // No UDS variant in EndPoint; use default Ipv4Endpoint as hint
        accept_handler_(client_fd, Ipv4Endpoint{});
    } else {
        ::close(client_fd);
    }
}

} // namespace net
} // namespace hpactor
