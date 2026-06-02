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

#include <hpactor/net/udp_transport.hpp>

#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/net/gossip_membership.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <thread>

namespace hpactor::net {

RealUdpTransport::RealUdpTransport(EventLoop* loop)
    : loop_(loop), recv_buffer_(kGossipMaxMsgSize) {}

RealUdpTransport::~RealUdpTransport() {
    close();
}

bool RealUdpTransport::bind(uint16_t port) {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0)
        return false;

    int reuse = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock_);
        sock_ = -1;
        return false;
    }

    // Spawn a dedicated polling thread for receiving UDP data.
    // We do NOT use the EventLoop's kqueue/epoll for UDP because kevent()
    // on macOS does not reliably detect EVFILT_READ on non-blocking UDP
    // sockets registered via add_fd().  A dedicated poll() + recvfrom()
    // loop avoids the issue entirely.
    running_.store(true);
    recv_thread_ = std::thread([this]() {
        while (running_.load()) {
            struct pollfd pfd;
            pfd.fd = sock_;
            pfd.events = POLLIN;
            int ready = poll(&pfd, 1, 100);
            if (ready <= 0 || !running_.load())
                continue;

            struct sockaddr_in src_addr{};
            socklen_t src_addr_len = sizeof(src_addr);

            while (true) {
                ssize_t n = recvfrom(
                    sock_, recv_buffer_.data(), recv_buffer_.size(), MSG_DONTWAIT,
                    reinterpret_cast<struct sockaddr*>(&src_addr), &src_addr_len);
                if (n <= 0)
                    break;

                StreamBuffer data(recv_buffer_.data(),
                                  recv_buffer_.data() + static_cast<size_t>(n));

                std::string from_host;
                uint16_t from_port = 0;
                char ip_str[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str))) {
                    from_host = ip_str;
                }
                from_port = ntohs(src_addr.sin_port);

                if (receive_cb_) {
                    receive_cb_(data, from_host, from_port);
                }
            }
        }
    });

    return true;
}

void RealUdpTransport::send(const StreamBuffer& data, const EndPoint& dest) {
    FAULT_INJECT("hpactor.gossip.packet.loss") {
        return;
    }
    if (sock_ < 0 || data.empty())
        return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&dest)) {
        addr.sin_addr.s_addr = ipv4->addr;
        addr.sin_port = ipv4->port_nw;
    } else {
        return;
    }

    ::sendto(sock_, data.data(), data.size(), 0,
             reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
}

void RealUdpTransport::close() {
    running_.store(false);
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

void RealUdpTransport::set_receive_callback(ReceiveCallback cb) {
    receive_cb_ = std::move(cb);
}

} // namespace hpactor::net
