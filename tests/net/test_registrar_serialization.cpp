// tests/net/test_registrar_serialization.cpp
// Copyright 2026 HPActor Contributors

#include <hpactor/net/registrar.hpp>
#include <hpactor/net/registrar_serialization.hpp>

#include <cassert>
#include <cstring>
#include <iostream>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test 1: PbRegisterPayload round-trip (acceptors at top level per spec)
    {
        NodeEndpoint ep;
        ep.identity.endpoint = endpoint_ops::parse_endpoint("192.168.1.100:"
                                                            "5353");
        ep.identity.host = "192.168.1.100";
        ep.tcp_port = 5353;
        ep.identity.acceptors.push_back({8080, 1, 1, false});

        StreamBuffer serialized = serialize_register_payload(ep);
        PbRegisterPayload parsed;
        bool ok = parse_register_payload(serialized, parsed);
        assert(ok);
        assert(parsed.endpoint_info().endpoint() == "192.168.1.100:5353");
        assert(parsed.endpoint_info().host() == "192.168.1.100");
        assert(parsed.endpoint_info().tcp_port() == 5353);
        assert(parsed.acceptors_size() == 1);
        assert(parsed.acceptors(0).port() == 8080);
    }

    // Test 2: PbAcceptPayload round-trip
    {
        StreamBuffer serialized = serialize_accept_payload(0);
        PbAcceptPayload parsed;
        bool ok = parse_accept_payload(serialized, parsed);
        assert(ok);
        assert(parsed.error_code() == 0);
    }

    // Test 3: PbNodeJoinPayload round-trip
    {
        NodeEndpoint ep;
        ep.identity.endpoint = endpoint_ops::parse_endpoint("10.0.0.1:4000");
        ep.identity.host = "10.0.0.1";
        ep.tcp_port = 4000;

        StreamBuffer serialized = serialize_node_join_payload(ep);
        PbNodeJoinPayload parsed;
        bool ok = parse_node_join_payload(serialized, parsed);
        assert(ok);
        assert(parsed.endpoint_info().endpoint() == "10.0.0.1:4000");
        assert(parsed.endpoint_info().host() == "10.0.0.1");
        assert(parsed.endpoint_info().tcp_port() == 4000);
    }

    // Test 4: PbNodeLeavePayload round-trip
    {
        auto ep = endpoint_ops::parse_endpoint("10.0.0.2:5000");
        StreamBuffer serialized = serialize_node_leave_payload(ep);
        PbNodeLeavePayload parsed;
        bool ok = parse_node_leave_payload(serialized, parsed);
        assert(ok);
        assert(parsed.endpoint() == "10.0.0.2:5000");
    }

    // Test 5: PbResolveQueryPayload round-trip
    {
        auto ep = endpoint_ops::parse_endpoint("192.168.1.50:5353");
        StreamBuffer serialized = serialize_resolve_query_payload(ep);
        PbResolveQueryPayload parsed;
        bool ok = parse_resolve_query_payload(serialized, parsed);
        assert(ok);
        assert(parsed.target_endpoint() == "192.168.1.50:5353");
    }

    // Test 6: PbResolveResponsePayload round-trip (endpoint_info only, no
    // acceptors per spec)
    {
        NodeEndpoint ep;
        ep.identity.endpoint = endpoint_ops::parse_endpoint("192.168.1.50:"
                                                            "5353");
        ep.identity.host = "192.168.1.50";
        ep.tcp_port = 5353;

        StreamBuffer serialized = serialize_resolve_response_payload(ep);
        PbResolveResponsePayload parsed;
        bool ok = parse_resolve_response_payload(serialized, parsed);
        assert(ok);
        assert(parsed.endpoint_info().endpoint() == "192.168.1.50:5353");
        assert(parsed.endpoint_info().host() == "192.168.1.50");
        assert(parsed.endpoint_info().tcp_port() == 5353);
    }

    // Test 7: Malformed data handling
    {
        StreamBuffer malformed = {0x00, 0x01, 0x02}; // Too short
        PbRegisterPayload parsed;
        bool ok = parse_register_payload(malformed, parsed);
        assert(!ok); // ParseFromArray returns false on error
        // Message should be empty/default on parse failure
        assert(parsed.endpoint_info().endpoint().empty());
    }

    std::cout << "All registrar_serialization tests passed" << std::endl;
    return 0;
}