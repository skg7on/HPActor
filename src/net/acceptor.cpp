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

Acceptor::Acceptor(EventLoop* loop) : loop_(loop), listening_fd_(-1) {}

Acceptor::~Acceptor() {
    close();
}

bool Acceptor::listen(uint16_t port, uint16_t port_range) {
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

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // Try binding, with port range fallback
    bool bound = false;
    for (uint16_t p = port; p < port + port_range + 1; ++p) {
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
    bound_port_ = port;

    // Register with event loop
    if (loop_) {
        loop_->add_fd(listening_fd_, EventLoop::Event::Read);
    }

    return true;
}

bool Acceptor::listen_unix_domain(const std::string& path) {
    // Remove stale socket file if it exists
    ::unlink(path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
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
    bound_port_ = 0; // UDS has no port
    uds_path_ = path;

    if (loop_) {
        loop_->add_fd(listening_fd_, EventLoop::Event::Read);
    }

    return true;
}

void Acceptor::close_unix_domain() {
    if (!uds_path_.empty()) {
        ::unlink(uds_path_.c_str());
        uds_path_.clear(); // Reset so uds_path() returns empty after close
    }
    close(); // Closes the fd and sets listening_fd_ = -1
}

void Acceptor::close() {
    if (listening_fd_ >= 0) {
        if (loop_) {
            loop_->remove_fd(listening_fd_);
        }
        ::close(listening_fd_);
        listening_fd_ = -1;
    }
    uds_path_.clear();
}

void Acceptor::set_accept_handler(accept_handler handler) {
    accept_handler_ = std::move(handler);
}

void Acceptor::handle_read() {
    struct sockaddr_in client_addr;
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
        // Convert IP to string (port unknown, use 0 as placeholder)
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
        Ipv4Endpoint endpoint_hint{client_addr.sin_addr.s_addr, 0};
        accept_handler_(client_fd, endpoint_hint);
    } else {
        ::close(client_fd);
    }
}

} // namespace net
} // namespace hpactor
