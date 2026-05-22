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

#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/registrar.hpp>

#include <arpa/inet.h>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

TEST(RegistrarConnectionTest, FramingEncoding) {
    StreamBuffer payload;
    StreamBuffer message;
    message.resize(10); // TcpHeaderSize = 10

    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = TcpRegistrarVersion;
    message[5] = static_cast<uint8_t>(TcpMessageType::Register);
    uint32_t len_be = htonl(0); // empty payload
    memcpy(message.data() + 6, &len_be, 4);

    EXPECT_EQ(message[4], TcpRegistrarVersion);
    EXPECT_EQ(static_cast<TcpMessageType>(message[5]), TcpMessageType::Register);

    uint32_t parsed_len;
    memcpy(&parsed_len, message.data() + 6, 4);
    parsed_len = ntohl(parsed_len);
    EXPECT_EQ(parsed_len, 0u);
}

TEST(RegistrarConnectionTest, TcpMessageTypeEnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(TcpMessageType::Register), 0x01);
    EXPECT_EQ(static_cast<uint8_t>(TcpMessageType::Heartbeat), 0x02);
    EXPECT_EQ(static_cast<uint8_t>(TcpMessageType::NodeJoin), 0x03);
    EXPECT_EQ(static_cast<uint8_t>(TcpMessageType::NodeLeave), 0x04);
    EXPECT_EQ(static_cast<uint8_t>(TcpMessageType::Accept), 0x05);
    EXPECT_EQ(static_cast<uint8_t>(TcpMessageType::Error), 0x06);
}

TEST(RegistrarConnectionTest, EndpointParsingIpv4) {
    std::string endpoint_str = "127.0.0.1:12345";
    EndPoint ep = endpoint_ops::parse_endpoint(endpoint_str);
    EXPECT_TRUE(std::holds_alternative<Ipv4Endpoint>(ep));
    auto ipv4 = std::get<Ipv4Endpoint>(ep);
    EXPECT_EQ(ipv4.port(), 12345u);
}
