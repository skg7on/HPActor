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

#include <gtest/gtest.h>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/registrar_serialization.hpp>

#include <cstring>

using namespace hpactor;
using namespace hpactor::net;

TEST(RegistrarSerializationTest, PbRegisterPayloadRoundTrip) {
    NodeEndpoint ep;
    ep.identity.endpoint = endpoint_ops::parse_endpoint("192.168.1.100:5353");
    ep.identity.host = "192.168.1.100";
    ep.tcp_port = 5353;
    ep.identity.acceptors.push_back({8080, 1, 1, false});

    StreamBuffer serialized = serialize_register_payload(ep);
    PbRegisterPayload parsed;
    bool ok = parse_register_payload(serialized, parsed);
    EXPECT_TRUE(ok);
    EXPECT_EQ(parsed.endpoint_info().endpoint(), "192.168.1.100:5353");
    EXPECT_EQ(parsed.endpoint_info().host(), "192.168.1.100");
    EXPECT_EQ(parsed.endpoint_info().tcp_port(), 5353u);
    EXPECT_EQ(parsed.acceptors_size(), 1);
    EXPECT_EQ(parsed.acceptors(0).port(), 8080u);
}

TEST(RegistrarSerializationTest, PbAcceptPayloadRoundTrip) {
    StreamBuffer serialized = serialize_accept_payload(0);
    PbAcceptPayload parsed;
    bool ok = parse_accept_payload(serialized, parsed);
    EXPECT_TRUE(ok);
    EXPECT_EQ(parsed.error_code(), 0);
}

TEST(RegistrarSerializationTest, PbNodeJoinPayloadRoundTrip) {
    NodeEndpoint ep;
    ep.identity.endpoint = endpoint_ops::parse_endpoint("10.0.0.1:4000");
    ep.identity.host = "10.0.0.1";
    ep.tcp_port = 4000;

    StreamBuffer serialized = serialize_node_join_payload(ep);
    PbNodeJoinPayload parsed;
    bool ok = parse_node_join_payload(serialized, parsed);
    EXPECT_TRUE(ok);
    EXPECT_EQ(parsed.endpoint_info().endpoint(), "10.0.0.1:4000");
    EXPECT_EQ(parsed.endpoint_info().host(), "10.0.0.1");
    EXPECT_EQ(parsed.endpoint_info().tcp_port(), 4000u);
}

TEST(RegistrarSerializationTest, PbNodeLeavePayloadRoundTrip) {
    auto ep = endpoint_ops::parse_endpoint("10.0.0.2:5000");
    StreamBuffer serialized = serialize_node_leave_payload(ep);
    PbNodeLeavePayload parsed;
    bool ok = parse_node_leave_payload(serialized, parsed);
    EXPECT_TRUE(ok);
    EXPECT_EQ(parsed.endpoint(), "10.0.0.2:5000");
}

TEST(RegistrarSerializationTest, PbResolveQueryPayloadRoundTrip) {
    auto ep = endpoint_ops::parse_endpoint("192.168.1.50:5353");
    StreamBuffer serialized = serialize_resolve_query_payload(ep);
    PbResolveQueryPayload parsed;
    bool ok = parse_resolve_query_payload(serialized, parsed);
    EXPECT_TRUE(ok);
    EXPECT_EQ(parsed.target_endpoint(), "192.168.1.50:5353");
}

TEST(RegistrarSerializationTest, PbResolveResponsePayloadRoundTrip) {
    NodeEndpoint ep;
    ep.identity.endpoint = endpoint_ops::parse_endpoint("192.168.1.50:5353");
    ep.identity.host = "192.168.1.50";
    ep.tcp_port = 5353;

    StreamBuffer serialized = serialize_resolve_response_payload(ep);
    PbResolveResponsePayload parsed;
    bool ok = parse_resolve_response_payload(serialized, parsed);
    EXPECT_TRUE(ok);
    EXPECT_EQ(parsed.endpoint_info().endpoint(), "192.168.1.50:5353");
    EXPECT_EQ(parsed.endpoint_info().host(), "192.168.1.50");
    EXPECT_EQ(parsed.endpoint_info().tcp_port(), 5353u);
}

TEST(RegistrarSerializationTest, MalformedDataHandling) {
    StreamBuffer malformed = {0x00, 0x01, 0x02};
    PbRegisterPayload parsed;
    bool ok = parse_register_payload(malformed, parsed);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(parsed.endpoint_info().endpoint().empty());
}
