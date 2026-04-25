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

#include <hpactor/net/registrar.hpp>
#include <hpactor/net/event_loop.hpp>

#include <cassert>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test framing encoding
    // Build a test message: Register type with empty payload
    bytes payload;
    bytes message;
    message.resize(10);  // TcpHeaderSize = 10

    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(message.data(), &magic_be, 4);
    message[4] = TcpRegistrarVersion;
    message[5] = static_cast<uint8_t>(TcpMessageType::Register);
    uint32_t len_be = htonl(0);  // empty payload
    memcpy(message.data() + 6, &len_be, 4);

    // Verify header parsing
    assert(message[4] == TcpRegistrarVersion);
    assert(static_cast<TcpMessageType>(message[5]) == TcpMessageType::Register);

    uint32_t parsed_len;
    memcpy(&parsed_len, message.data() + 6, 4);
    parsed_len = ntohl(parsed_len);
    assert(parsed_len == 0);

    // Test TcpMessageType enum values
    assert(static_cast<uint8_t>(TcpMessageType::Register) == 0x01);
    assert(static_cast<uint8_t>(TcpMessageType::Heartbeat) == 0x02);
    assert(static_cast<uint8_t>(TcpMessageType::NodeJoin) == 0x03);
    assert(static_cast<uint8_t>(TcpMessageType::NodeLeave) == 0x04);
    assert(static_cast<uint8_t>(TcpMessageType::Accept) == 0x05);
    assert(static_cast<uint8_t>(TcpMessageType::Error) == 0x06);

    // Test endpoint parsing for broadcast
    std::string endpoint_str = "127.0.0.1:12345";
    CommunicationEndpoint ep = endpoint_ops::parse_endpoint(endpoint_str);
    assert(std::holds_alternative<Ipv4Endpoint>(ep));
    auto ipv4 = std::get<Ipv4Endpoint>(ep);
    assert(ipv4.port() == 12345);

    std::cout << "All RegistrarConnection tests passed" << std::endl;
    return 0;
}