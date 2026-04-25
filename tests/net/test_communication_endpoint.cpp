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

#include <array>
#include <cassert>
#include <hpactor/types/types.hpp>

int main() {
    using namespace hpactor;

    // Test IPv4Endpoint construction
    Ipv4Endpoint ipv4{0x01010101, htons(5353)}; // 1.1.1.1 in network order
    assert(ipv4.port() == 5353);
    assert(ipv4.is_ipv4() == true);
    assert(ipv4.is_ipv6() == false);

    // Test IPv6Endpoint construction
    std::array<uint8_t, 16> loopback_arr{};
    loopback_arr[15] = 1; // ::1
    Ipv6Endpoint ipv6{loopback_arr, htons(8080)};
    assert(ipv6.port() == 8080);
    assert(ipv6.is_ipv6() == true);
    assert(ipv6.is_ipv4() == false);

    // Test CommunicationEndpoint variant
    CommunicationEndpoint ep = ipv4;
    assert(std::holds_alternative<Ipv4Endpoint>(ep));
    assert(!std::holds_alternative<Ipv6Endpoint>(ep));

    // Test is_loopback
    Ipv4Endpoint loopback4{0x7F000001, 1234}; // 127.0.0.1
    assert(loopback4.is_loopback() == true);
    assert((Ipv4Endpoint{0xC0A80001, 1234}.is_loopback() == false)); // 192.168.0.1

    std::array<uint8_t, 16> loopback6_arr{};
    loopback6_arr[15] = 1;
    assert((Ipv6Endpoint{loopback6_arr, 1234}.is_loopback() == true));

    // Test is_private_network
    assert((Ipv4Endpoint{0x0A000001, 1234}.is_private_network() == true)); // 10.0.0.1
    assert((Ipv4Endpoint{0xAC100001, 1234}.is_private_network() == true)); // 172.16.0.1
    assert((Ipv4Endpoint{0xC0A80001, 1234}.is_private_network() == true)); // 192.168.0.1
    assert((Ipv4Endpoint{0x08080808, 1234}.is_private_network() == false)); // 8.8.8.8

    // Test sockaddr conversion
    sockaddr_in addr4;
    ipv4.to_sockaddr(&addr4);
    assert(addr4.sin_family == AF_INET);
    assert(addr4.sin_port == htons(5353));
    assert(addr4.sin_addr.s_addr == 0x01010101);

    sockaddr_in6 addr6;
    ipv6.to_sockaddr(&addr6);
    assert(addr6.sin6_family == AF_INET6);
    assert(addr6.sin6_port == htons(8080));

    // Test hash equality
    Ipv4Endpoint ipv4_copy{0x01010101, htons(5353)};
    assert(std::hash<Ipv4Endpoint>{}(ipv4) == std::hash<Ipv4Endpoint>{}(ipv4_copy));

    // Test endpoint_ops::protocol
    assert(endpoint_ops::protocol(ep) == Protocol::IPv4);
    CommunicationEndpoint ep6 = ipv6;
    assert(endpoint_ops::protocol(ep6) == Protocol::IPv6);

    // Test endpoint_ops::address_family
    assert(endpoint_ops::address_family(ep) == AF_INET);
    assert(endpoint_ops::address_family(ep6) == AF_INET6);

    // Test endpoint_ops::to_string
    assert(endpoint_ops::to_string(ep) == "1.1.1.1:5353");
    assert(endpoint_ops::to_string(ep6) == "[::1]:8080");

    return 0;
}