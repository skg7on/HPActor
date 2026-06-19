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

#include <hpactor/msg/frame.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace hpactor;
using namespace hpactor::net;

// =============================================================================
// Test 1: EndPoint operations (IPv4 and IPv6)
// =============================================================================

TEST(NetUnitFinalTest, Ipv4EndpointDefaultConstruction) {
    Ipv4Endpoint ep;
    EXPECT_EQ(ep.addr, 0u);
    EXPECT_EQ(ep.port_nw, 0u);
    EXPECT_EQ(ep.port(), 0u);
    EXPECT_TRUE(ep.is_ipv4());
    EXPECT_FALSE(ep.is_ipv6());
    EXPECT_TRUE(ep.is_unspecified());
    EXPECT_FALSE(ep.is_loopback());
}

TEST(NetUnitFinalTest, Ipv4EndpointLoopback) {
    // 127.0.0.1 in network byte order
    Ipv4Endpoint ep{0x7F000001, htons(8080)};
    EXPECT_TRUE(ep.is_loopback());
    EXPECT_FALSE(ep.is_private_network());
    EXPECT_FALSE(ep.is_unspecified());
    EXPECT_EQ(ep.port(), 8080u);
}

TEST(NetUnitFinalTest, Ipv4EndpointPrivateNetwork) {
    // 10.0.0.1
    Ipv4Endpoint ep10{0x0A000001, htons(1234)};
    EXPECT_TRUE(ep10.is_private_network());

    // 172.16.0.1
    Ipv4Endpoint ep172{0xAC100001, htons(1234)};
    EXPECT_TRUE(ep172.is_private_network());

    // 192.168.0.1
    Ipv4Endpoint ep192{0xC0A80001, htons(1234)};
    EXPECT_TRUE(ep192.is_private_network());
}

TEST(NetUnitFinalTest, Ipv4EndpointEquality) {
    Ipv4Endpoint ep1{0x7F000001, htons(8080)};
    Ipv4Endpoint ep2{0x7F000001, htons(8080)};
    Ipv4Endpoint ep3{0x7F000001, htons(9090)};

    EXPECT_EQ(ep1, ep2);
    EXPECT_NE(ep1, ep3);
}

TEST(NetUnitFinalTest, Ipv6EndpointLoopback) {
    std::array<uint8_t, 16> addr{};
    addr[15] = 1; // ::1
    Ipv6Endpoint ep{addr, htons(8080)};
    EXPECT_TRUE(ep.is_loopback());
    EXPECT_FALSE(ep.is_private_network());
    EXPECT_FALSE(ep.is_unspecified());
    EXPECT_EQ(ep.port(), 8080u);
}

TEST(NetUnitFinalTest, Ipv6EndpointUnspecified) {
    std::array<uint8_t, 16> addr{};
    Ipv6Endpoint ep{addr, 0};
    EXPECT_TRUE(ep.is_unspecified());
}

TEST(NetUnitFinalTest, EndpointParseAndToString) {
    auto ep1 = endpoint_ops::parse_endpoint("192.168.1.1:8080");
    auto str1 = endpoint_ops::to_string(ep1);
    EXPECT_FALSE(str1.empty());

    auto ep2 = endpoint_ops::parse_endpoint("10.0.0.1:0");
    auto str2 = endpoint_ops::to_string(ep2);
    EXPECT_FALSE(str2.empty());

    // Both are IPv4
    EXPECT_EQ(endpoint_ops::protocol(ep1), Protocol::IPv4);
    EXPECT_EQ(endpoint_ops::protocol(ep2), Protocol::IPv4);
}

TEST(NetUnitFinalTest, EndpointHash) {
    auto ep1 = endpoint_ops::parse_endpoint("192.168.1.1:8080");
    auto ep2 = endpoint_ops::parse_endpoint("192.168.1.1:8080");
    auto ep3 = endpoint_ops::parse_endpoint("192.168.1.2:8080");

    std::hash<EndPoint> hasher;
    EXPECT_EQ(hasher(ep1), hasher(ep2));
    EXPECT_NE(hasher(ep1), hasher(ep3));
}

// =============================================================================
// Test 2: HTTP types validation
// =============================================================================

TEST(NetUnitFinalTest, HttpMethodToString) {
    EXPECT_STREQ(to_string(HttpMethod::GET), "GET");
    EXPECT_STREQ(to_string(HttpMethod::POST), "POST");
    EXPECT_STREQ(to_string(HttpMethod::PUT), "PUT");
    EXPECT_STREQ(to_string(HttpMethod::DELETE), "DELETE");
    EXPECT_STREQ(to_string(HttpMethod::PATCH), "PATCH");
    EXPECT_STREQ(to_string(HttpMethod::HEAD), "HEAD");
    EXPECT_STREQ(to_string(HttpMethod::OPTIONS), "OPTIONS");
}

TEST(NetUnitFinalTest, HttpMethodFromString) {
    EXPECT_EQ(method_from_string("GET", 3), HttpMethod::GET);
    EXPECT_EQ(method_from_string("POST", 4), HttpMethod::POST);
    EXPECT_EQ(method_from_string("PUT", 3), HttpMethod::PUT);
    EXPECT_EQ(method_from_string("DELETE", 6), HttpMethod::DELETE);
    EXPECT_EQ(method_from_string("PATCH", 5), HttpMethod::PATCH);
    EXPECT_EQ(method_from_string("HEAD", 4), HttpMethod::HEAD);
    EXPECT_EQ(method_from_string("OPTIONS", 7), HttpMethod::OPTIONS);
}

TEST(NetUnitFinalTest, HttpMethodFromStringUnknownReturnsGet) {
    EXPECT_EQ(method_from_string("UNKNOWN", 7), HttpMethod::GET);
}

TEST(NetUnitFinalTest, HttpStatusCodeValues) {
    EXPECT_EQ(static_cast<uint16_t>(HttpStatusCode::OK), 200u);
    EXPECT_EQ(static_cast<uint16_t>(HttpStatusCode::NotFound), 404u);
    EXPECT_EQ(static_cast<uint16_t>(HttpStatusCode::InternalError), 500u);
}

TEST(NetUnitFinalTest, ReasonPhraseForAllCodes) {
    EXPECT_STREQ(reason_phrase(HttpStatusCode::OK), "OK");
    EXPECT_STREQ(reason_phrase(HttpStatusCode::NotFound), "Not Found");
    EXPECT_STREQ(reason_phrase(HttpStatusCode::InternalError),
                 "Internal Server Error");
    EXPECT_STREQ(reason_phrase(HttpStatusCode::BadRequest), "Bad Request");
    // Unknown code returns "Unknown"
    EXPECT_STREQ(reason_phrase(static_cast<HttpStatusCode>(999)), "Unknown");
}

TEST(NetUnitFinalTest, HttpRequestDefaultConstruction) {
    HttpRequest req;
    EXPECT_EQ(req.method, HttpMethod::GET);
    EXPECT_TRUE(req.path.empty());
    EXPECT_EQ(req.http_major, 1);
    EXPECT_EQ(req.http_minor, 1);
    EXPECT_TRUE(req.headers.empty());
}

TEST(NetUnitFinalTest, HttpRequestHeaderLookup) {
    HttpRequest req;
    req.headers.push_back({"content-type", "application/json"});
    req.headers.push_back({"authorization", "Bearer token"});

    EXPECT_EQ(req.header("content-type").value_or("none"), "application/json");
    EXPECT_EQ(req.header("authorization").value_or("none"), "Bearer token");
    EXPECT_EQ(req.header("x-custom").value_or("none"), "none");
    EXPECT_EQ(req.content_type().value_or("none"), "application/json");
}

TEST(NetUnitFinalTest, HttpResponseStaticFactories) {
    auto resp_ok = HttpResponse::ok();
    EXPECT_EQ(resp_ok.status_code, HttpStatusCode::OK);

    auto resp_created = HttpResponse::created();
    EXPECT_EQ(resp_created.status_code, HttpStatusCode::Created);

    auto resp_no_content = HttpResponse::no_content();
    EXPECT_EQ(resp_no_content.status_code, HttpStatusCode::NoContent);

    auto resp_not_found = HttpResponse::not_found();
    EXPECT_EQ(resp_not_found.status_code, HttpStatusCode::NotFound);

    auto resp_err =
        HttpResponse::error(HttpStatusCode::BadGateway, "downstream failed");
    EXPECT_EQ(resp_err.status_code, HttpStatusCode::BadGateway);
    EXPECT_EQ(resp_err.headers.size(), 1u);
    EXPECT_EQ(resp_err.headers[0].name, "Content-Type");
    EXPECT_EQ(resp_err.headers[0].value, "text/plain");
}

TEST(NetUnitFinalTest, HttpRequestQueryParams) {
    HttpRequest req;
    req.query_params["key"] = "value";
    req.query_params["page"] = "1";

    EXPECT_EQ(req.query_params.at("key"), "value");
    EXPECT_EQ(req.query_params.at("page"), "1");
    EXPECT_EQ(req.query_params.size(), 2u);
}

// =============================================================================
// Test 3: Frame encode/decode edge cases
// =============================================================================

TEST(NetUnitFinalTest, WireFrameEmptyPayloadRoundtrip) {
    ActorAddress sender(endpoint_ops::parse_endpoint("127.0.0.1:0"), 1,
                        ActorId{1}, 0);
    ActorAddress receiver(endpoint_ops::parse_endpoint("127.0.0.1:0"), 2,
                          ActorId{2}, 0);

    WireFrame f;
    to_proto(f.pb_frame.mutable_sender(), sender);
    to_proto(f.pb_frame.mutable_receiver(), receiver);
    f.pb_frame.set_flags(WireFrame::NoDrop);
    f.pb_frame.set_message_id(42);

    StreamBuffer encoded = f.encode();
    EXPECT_FALSE(encoded.empty());

    WireFrame decoded = WireFrame::decode(encoded);
    EXPECT_EQ(decoded.pb_frame.message_id(), 42u);
    EXPECT_EQ(decoded.pb_frame.flags(), WireFrame::NoDrop);
}

TEST(NetUnitFinalTest, WireFrameRpcFlags) {
    WireFrame f;
    f.pb_frame.set_flags(WireFrame::RpcRequest | WireFrame::RpcIdempotent);
    f.pb_frame.set_message_id(12345);

    StreamBuffer encoded = f.encode();
    WireFrame decoded = WireFrame::decode(encoded);

    EXPECT_TRUE(decoded.pb_frame.flags() & WireFrame::RpcRequest);
    EXPECT_TRUE(decoded.pb_frame.flags() & WireFrame::RpcIdempotent);
    EXPECT_EQ(decoded.pb_frame.message_id(), 12345u);
}

TEST(NetUnitFinalTest, WireFrameDecodeFromSpan) {
    WireFrame f;
    f.pb_frame.set_message_id(999);
    f.pb_frame.set_payload("data", 4u);

    StreamBuffer encoded = f.encode();
    std::span<const uint8_t> span(encoded.data(), encoded.size());

    WireFrame decoded = WireFrame::decode(span);
    EXPECT_EQ(decoded.pb_frame.message_id(), 999u);
    EXPECT_EQ(decoded.pb_frame.payload(), "data");
}

TEST(NetUnitFinalTest, WireFrameDecodeMalformedTruncated) {
    // Only 4 bytes — too short for even the header
    StreamBuffer truncated = {0x48, 0x50, 0x41, 0x43}; // "HPAC" in LE
    WireFrame f = WireFrame::decode(truncated);
    // Should not crash; magic header should be populated
    EXPECT_EQ(f.pb_frame.message_id(), 0u);
}

TEST(NetUnitFinalTest, WireFrameMagicHeaderConstant) {
    EXPECT_EQ(WireFrame::MagicHeader, 0x43415048u);
    EXPECT_EQ(WireFrame::HeaderSize, 8u);
}

// =============================================================================
// Test 4: Connection lifecycle states
// =============================================================================

TEST(NetUnitFinalTest, ConnectionStateValues) {
    EXPECT_EQ(static_cast<int>(ConnectionState::Disconnected), 0);
    EXPECT_EQ(static_cast<int>(ConnectionState::Connecting), 1);
    EXPECT_EQ(static_cast<int>(ConnectionState::Handshake), 2);
    EXPECT_EQ(static_cast<int>(ConnectionState::Connected), 3);
    EXPECT_EQ(static_cast<int>(ConnectionState::Error), 4);
}

TEST(NetUnitFinalTest, TransportErrorValues) {
    EXPECT_EQ(static_cast<int>(TransportError::Success), 0);
    EXPECT_EQ(static_cast<int>(TransportError::ConnectionFailed), 1);
    EXPECT_EQ(static_cast<int>(TransportError::Timeout), 2);
    EXPECT_EQ(static_cast<int>(TransportError::SerializationFailed), 3);
    EXPECT_EQ(static_cast<int>(TransportError::BufferOverflow), 4);
    EXPECT_EQ(static_cast<int>(TransportError::NotConnected), 5);
}

// =============================================================================
// Test 5: Transport config defaults — verified via enum/struct defaults
// =============================================================================

TEST(NetUnitFinalTest, TransportSendResultValues) {
    EXPECT_EQ(static_cast<int>(TransportSendResult::Sent), 0);
    EXPECT_EQ(static_cast<int>(TransportSendResult::NotConnected), 1);
    EXPECT_EQ(static_cast<int>(TransportSendResult::QueueFull), 2);
    EXPECT_EQ(static_cast<int>(TransportSendResult::CircuitOpen), 3);
    EXPECT_EQ(static_cast<int>(TransportSendResult::EncodeError), 4);
    EXPECT_EQ(static_cast<int>(TransportSendResult::ShuttingDown), 5);
    EXPECT_EQ(static_cast<int>(TransportSendResult::WriteError), 6);
}

// =============================================================================
// Test 6: Service discovery interface
// =============================================================================

namespace {

struct FinalTestDiscovery : public IServiceDiscovery {
    void start() override {}
    void stop() override {}
    std::vector<Member> discover_all() const override {
        return {};
    }
    const Member* discover(EndPoint) const override {
        return nullptr;
    }
    void announce(Member) override {}
    void on_member_change(MemberChangeCallback) override {}
    std::string backend_name() const override {
        return "final-test";
    }
};

} // namespace

TEST(NetUnitFinalTest, ServiceDiscoveryBackendName) {
    FinalTestDiscovery ftd;
    EXPECT_EQ(ftd.backend_name(), "final-test");
}

TEST(NetUnitFinalTest, ServiceDiscoveryDefaultRawMembersNull) {
    FinalTestDiscovery ftd;
    EXPECT_EQ(ftd.raw_members(), nullptr);
}

TEST(NetUnitFinalTest, ServiceDiscoveryMemberDefaults) {
    Member m;
    EXPECT_EQ(m.status, MemberStatus::Alive);
    EXPECT_EQ(m.incarnation, 0u);
    EXPECT_TRUE(m.actor_types.empty());
    EXPECT_TRUE(m.identity.host.empty());
}

TEST(NetUnitFinalTest, StaticDiscoveryDiscoverAllEmpty) {
    StaticDiscovery sd({});
    auto all = sd.discover_all();
    EXPECT_TRUE(all.empty());
}

TEST(NetUnitFinalTest, StaticDiscoveryBackendName) {
    StaticDiscovery sd({});
    EXPECT_EQ(sd.backend_name(), "static");
}

TEST(NetUnitFinalTest, MemberStatusValues) {
    EXPECT_EQ(static_cast<uint8_t>(MemberStatus::Alive), 0u);
    EXPECT_EQ(static_cast<uint8_t>(MemberStatus::Suspicious), 1u);
    EXPECT_EQ(static_cast<uint8_t>(MemberStatus::Dead), 2u);
    EXPECT_EQ(static_cast<uint8_t>(MemberStatus::Left), 3u);
}

// =============================================================================
// ActorLocationCache additional coverage
// =============================================================================

TEST(NetUnitFinalTest, ActorLocationCacheSizeIsInitiallyZero) {
    ActorLocationCache cache;
    EXPECT_FALSE(cache.get(ActorId{1}).has_value());
}

TEST(NetUnitFinalTest, ActorLocationCachePutWithCustomTtl) {
    ActorLocationCache cache;
    ActorId id(1);
    auto ep = endpoint_ops::parse_endpoint("192.168.1.1:9000");

    cache.put(id, ep, std::chrono::seconds(3600));
    auto result = cache.get(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, ep);
}
