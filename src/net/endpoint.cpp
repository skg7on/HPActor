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

} // namespace endpoint_ops
} // namespace hpactor