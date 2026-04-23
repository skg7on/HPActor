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

#include <hpactor/types/types.hpp>
#include <arpa/inet.h>
#include <cstdio>

namespace hpactor {
namespace endpoint_ops {

Protocol protocol(const CommunicationEndpoint& ep) {
    if (std::holds_alternative<Ipv4Endpoint>(ep)) return Protocol::IPv4;
    return Protocol::IPv6;
}

int address_family(const CommunicationEndpoint& ep) {
    return std::holds_alternative<Ipv4Endpoint>(ep) ? AF_INET : AF_INET6;
}

std::string to_string(const CommunicationEndpoint& ep) {
    char buf[64];
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
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

CommunicationEndpoint parse_endpoint(const NodeId& node_id) {
    // Empty node_id means local node - return loopback endpoint
    if (node_id.empty()) {
        struct in_addr loopback_addr;
        inet_pton(AF_INET, "127.0.0.1", &loopback_addr);
        // inet_pton returns host byte order in s_addr, convert to network byte order
        return Ipv4Endpoint{htonl(loopback_addr.s_addr), 0};
    }

    // Parse "host:port" format
    std::string host = node_id_host(node_id);
    uint16_t port = node_id_port(node_id);

    // Convert host to IPv4 address
    struct in_addr addr;
    if (inet_pton(AF_INET, host.c_str(), &addr) != 1) {
        // Failed to parse - return unspecified address (not loopback)
        // This ensures is_local() returns false for unparseable addresses
        return Ipv4Endpoint{0, 0};
    }

    // Convert port to network byte order
    uint16_t port_nw = htons(port);
    // inet_pton returns host byte order in s_addr, convert to network byte order
    return Ipv4Endpoint{htonl(addr.s_addr), port_nw};
}

} // namespace endpoint_ops
} // namespace hpactor