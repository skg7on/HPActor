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
#include <hpactor/types/types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

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

    if (loop_) {
        loop_->add_fd(sock_, EventLoop::Event::Read);
        loop_->set_read_handler(sock_, [this](int /*fd*/) {
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
        });
    }
    return true;
}

void RealUdpTransport::send(const StreamBuffer& data, const EndPoint& dest) {
    FAULT_INJECT("hpactor.gossip.packet.loss") {
        return;
    }
    if (sock_ < 0 || data.empty())
        return;

    if (loop_) {
        struct iovec iov;
        iov.iov_base = const_cast<uint8_t*>(data.data());
        iov.iov_len = data.size();

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&dest)) {
            addr.sin_addr.s_addr = ipv4->addr;
            addr.sin_port = ipv4->port_nw;
        }

        loop_->backend()->async_sendto(
            sock_, &iov, 1, reinterpret_cast<const sockaddr*>(&addr),
            sizeof(addr), ActorId(0), static_cast<uint32_t>(OpType::SendTo));
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&dest)) {
            addr.sin_addr.s_addr = ipv4->addr;
            addr.sin_port = ipv4->port_nw;
        }
        ::sendto(sock_, data.data(), data.size(), 0,
                 reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    }
}

void RealUdpTransport::close() {
    if (sock_ >= 0) {
        if (loop_) {
            loop_->clear_read_handler(sock_);
            loop_->remove_fd(sock_);
        }
        ::close(sock_);
        sock_ = -1;
    }
}

void RealUdpTransport::set_receive_callback(ReceiveCallback cb) {
    receive_cb_ = std::move(cb);
}

} // namespace hpactor::net
