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
#include <gtest/gtest.h>
#include <hpactor/types/types.hpp>

using namespace hpactor;

TEST(CommunicationEndpointTest, Ipv4EndpointConstruction) {
    Ipv4Endpoint ipv4{0x01010101, htons(5353)};
    EXPECT_EQ(ipv4.port(), 5353u);
    EXPECT_TRUE(ipv4.is_ipv4());
    EXPECT_FALSE(ipv4.is_ipv6());
}

TEST(CommunicationEndpointTest, Ipv6EndpointConstruction) {
    std::array<uint8_t, 16> loopback_arr{};
    loopback_arr[15] = 1;
    Ipv6Endpoint ipv6{loopback_arr, htons(8080)};
    EXPECT_EQ(ipv6.port(), 8080u);
    EXPECT_TRUE(ipv6.is_ipv6());
    EXPECT_FALSE(ipv6.is_ipv4());
}

TEST(CommunicationEndpointTest, EndPointVariant) {
    Ipv4Endpoint ipv4{0x01010101, htons(5353)};
    EndPoint ep = ipv4;
    EXPECT_TRUE(std::holds_alternative<Ipv4Endpoint>(ep));
    EXPECT_FALSE(std::holds_alternative<Ipv6Endpoint>(ep));
}

TEST(CommunicationEndpointTest, IsLoopback) {
    Ipv4Endpoint loopback4{0x7F000001, 1234};
    EXPECT_TRUE(loopback4.is_loopback());
    EXPECT_FALSE((Ipv4Endpoint{0xC0A80001, 1234}.is_loopback()));

    std::array<uint8_t, 16> loopback6_arr{};
    loopback6_arr[15] = 1;
    EXPECT_TRUE((Ipv6Endpoint{loopback6_arr, 1234}.is_loopback()));
}

TEST(CommunicationEndpointTest, IsPrivateNetwork) {
    EXPECT_TRUE((Ipv4Endpoint{0x0A000001, 1234}.is_private_network()));
    EXPECT_TRUE((Ipv4Endpoint{0xAC100001, 1234}.is_private_network()));
    EXPECT_TRUE((Ipv4Endpoint{0xC0A80001, 1234}.is_private_network()));
    EXPECT_FALSE((Ipv4Endpoint{0x08080808, 1234}.is_private_network()));
}

TEST(CommunicationEndpointTest, SockaddrConversion) {
    Ipv4Endpoint ipv4{0x01010101, htons(5353)};
    sockaddr_in addr4;
    ipv4.to_sockaddr(&addr4);
    EXPECT_EQ(addr4.sin_family, AF_INET);
    EXPECT_EQ(addr4.sin_port, htons(5353));
    EXPECT_EQ(addr4.sin_addr.s_addr, 0x01010101u);

    std::array<uint8_t, 16> loopback_arr{};
    loopback_arr[15] = 1;
    Ipv6Endpoint ipv6{loopback_arr, htons(8080)};
    sockaddr_in6 addr6;
    ipv6.to_sockaddr(&addr6);
    EXPECT_EQ(addr6.sin6_family, AF_INET6);
    EXPECT_EQ(addr6.sin6_port, htons(8080));
}

TEST(CommunicationEndpointTest, HashEquality) {
    Ipv4Endpoint ipv4{0x01010101, htons(5353)};
    Ipv4Endpoint ipv4_copy{0x01010101, htons(5353)};
    EXPECT_EQ(std::hash<Ipv4Endpoint>{}(ipv4), std::hash<Ipv4Endpoint>{}(ipv4_copy));
}

TEST(CommunicationEndpointTest, ProtocolAndAddressFamily) {
    Ipv4Endpoint ipv4{0x01010101, htons(5353)};
    EndPoint ep = ipv4;
    EXPECT_EQ(endpoint_ops::protocol(ep), Protocol::IPv4);
    EXPECT_EQ(endpoint_ops::address_family(ep), AF_INET);

    std::array<uint8_t, 16> loopback_arr{};
    loopback_arr[15] = 1;
    Ipv6Endpoint ipv6{loopback_arr, htons(8080)};
    EndPoint ep6 = ipv6;
    EXPECT_EQ(endpoint_ops::protocol(ep6), Protocol::IPv6);
    EXPECT_EQ(endpoint_ops::address_family(ep6), AF_INET6);
}

TEST(CommunicationEndpointTest, ToString) {
    Ipv4Endpoint ipv4{0x01010101, htons(5353)};
    EndPoint ep = ipv4;
    EXPECT_EQ(endpoint_ops::to_string(ep), "1.1.1.1:5353");

    std::array<uint8_t, 16> loopback_arr{};
    loopback_arr[15] = 1;
    Ipv6Endpoint ipv6{loopback_arr, htons(8080)};
    EndPoint ep6 = ipv6;
    EXPECT_EQ(endpoint_ops::to_string(ep6), "[::1]:8080");
}
