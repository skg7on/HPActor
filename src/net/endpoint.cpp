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

#include <arpa/inet.h>
#include <cstdio>
#include <hpactor/types/types.hpp>
#include <netdb.h>
#include <sys/socket.h>

namespace hpactor {
namespace endpoint_ops {

Protocol protocol(const EndPoint& ep) {
    if (std::holds_alternative<Ipv4Endpoint>(ep))
        return Protocol::IPv4;
    return Protocol::IPv6;
}

int address_family(const EndPoint& ep) {
    return std::holds_alternative<Ipv4Endpoint>(ep) ? AF_INET : AF_INET6;
}

std::string to_string(const EndPoint& ep) {
    char buf[64];
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
        // addr is in network byte order, pass directly to inet_ntoa
        struct in_addr addr;
        addr.s_addr = ipv4->addr;
        snprintf(buf, sizeof(buf), "%s:%u", inet_ntoa(addr), ipv4->port());
        return std::string(buf);
    }
    if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&ep)) {
        struct in6_addr addr;
        std::memcpy(addr.s6_addr, ipv6->addr.data(), 16);
        char addrbuf[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &addr, addrbuf, sizeof(addrbuf));
        snprintf(buf, sizeof(buf), "[%s]:%u", addrbuf, ipv6->port());
        return std::string(buf);
    }
    return "<invalid>";
}

EndPoint parse_endpoint(std::string_view node_id) {
    // Empty node_id means local node - return loopback endpoint
    if (node_id.empty()) {
        return LocalEndpoint;
    }

    // Parse "host:port" format
    auto colon_pos = node_id.find(':');
    if (colon_pos == std::string_view::npos) {
        return Ipv4Endpoint{0, 0}; // Unspecified on failure
    }
    std::string_view host = node_id.substr(0, colon_pos);
    std::string_view port_str = node_id.substr(colon_pos + 1);

    // Parse port manually (no exceptions)
    uint32_t port = 0;
    for (char c : port_str) {
        if (c >= '0' && c <= '9') {
            port = port * 10 + static_cast<uint32_t>(c - '0');
            if (port > 65535) {
                return Ipv4Endpoint{0, 0};
            }
        } else {
            return Ipv4Endpoint{0, 0};
        }
    }

    // Try numeric IPv4 address first
    struct in_addr addr;
    if (inet_pton(AF_INET, std::string(host).c_str(), &addr) == 1) {
        // inet_pton returns network byte order in s_addr
        return Ipv4Endpoint{addr.s_addr, htons(static_cast<uint16_t>(port))};
    }

    // Try numeric IPv6 address
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, std::string(host).c_str(), &addr6) == 1) {
        std::array<uint8_t, 16> bytes;
        std::memcpy(bytes.data(), addr6.s6_addr, 16);
        return Ipv6Endpoint{bytes, htons(static_cast<uint16_t>(port))};
    }

    // Fallback: try DNS resolution via getaddrinfo for IPv4
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = 0;
    struct addrinfo* result = nullptr;
    int ret = getaddrinfo(std::string(host).c_str(), nullptr, &hints, &result);
    if (ret == 0 && result != nullptr) {
        auto* ai = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
        // sin_addr.s_addr is in network byte order
        uint32_t addr_bytes = ai->sin_addr.s_addr;
        freeaddrinfo(result);
        return Ipv4Endpoint{addr_bytes, htons(static_cast<uint16_t>(port))};
    }

    // Try IPv6 via DNS
    if (result) {
        freeaddrinfo(result);
        result = nullptr;
    }
    hints.ai_family = AF_INET6;
    ret = getaddrinfo(std::string(host).c_str(), nullptr, &hints, &result);
    if (ret == 0 && result != nullptr) {
        auto* ai = reinterpret_cast<struct sockaddr_in6*>(result->ai_addr);
        std::array<uint8_t, 16> bytes;
        std::memcpy(bytes.data(), ai->sin6_addr.s6_addr, 16);
        freeaddrinfo(result);
        return Ipv6Endpoint{bytes, htons(static_cast<uint16_t>(port))};
    }
    if (result) {
        freeaddrinfo(result);
    }

    return Ipv4Endpoint{0, 0}; // Unspecified on failure
    if (result) {
        freeaddrinfo(result);
        result = nullptr;
    }
    hints.ai_family = AF_INET6;
    ret = getaddrinfo(std::string(host).c_str(), nullptr, &hints, &result);
    if (ret == 0 && result != nullptr) {
        auto* ai = reinterpret_cast<struct sockaddr_in6*>(result->ai_addr);
        std::array<uint8_t, 16> bytes;
        std::memcpy(bytes.data(), ai->sin6_addr.s6_addr, 16);
        freeaddrinfo(result);
        return Ipv6Endpoint{bytes, htons(static_cast<uint16_t>(port))};
    }
    if (result) {
        freeaddrinfo(result);
    }

    return Ipv4Endpoint{0, 0}; // Unspecified on failure
}

} // namespace endpoint_ops
} // namespace hpactor