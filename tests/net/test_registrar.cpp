#include <hpactor/net/registrar.hpp>

#include <cassert>
#include <iostream>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test AcceptorInfo defaults
    AcceptorInfo info;
    assert(info.port == 0);
    assert(info.handshake_version == 0);
    assert(info.protocol_version == 0);
    assert(info.tls_required == false);

    // Test RegistrarConfig defaults
    RegistrarConfig config;
    assert(config.udp_port == 5353);
    assert(config.tcp_port == 5353);
    assert(config.heartbeat_interval.count() == 5000);
    assert(config.expiration_timeout.count() == 15000);
    assert(config.probe_interval.count() == 30000);
    assert(config.static_routes.empty());
    assert(config.disable_server == false);

    // Test StaticRouteConfig defaults
    StaticRouteConfig route;
    assert(route.node_id == 0);
    assert(route.port == 0);

    // Test NodeEndpoint defaults
    NodeEndpoint ep;
    assert(ep.node_id == 0);
    assert(ep.tcp_port == 0);
    assert(ep.is_static_route == false);
    assert(ep.acceptors.empty());

    // Test NodeEndpoint with acceptors
    NodeEndpoint ep3;
    ep3.node_id = 1;
    ep3.host = "localhost";
    ep3.tcp_port = 9000;
    ep3.acceptors.push_back({9000, 1, 1, false});
    assert(ep3.acceptors.size() == 1);
    assert(ep3.acceptors[0].port == 9000);
    assert(ep3.acceptors[0].handshake_version == 1);
    assert(ep3.acceptors[0].protocol_version == 1);
    assert(ep3.acceptors[0].tls_required == false);

    // Test HostResolver with IP address (no DNS needed)
    HostResolver resolver;
    resolver.cache("192.168.1.1", "192.168.1.1", std::chrono::seconds(60));
    assert(resolver.get_cached("192.168.1.1") == "192.168.1.1");
    assert(resolver.get_cached("unknown") == "");

    // Test RegistrarMessageType enum values
    assert(static_cast<uint8_t>(RegistrarMessageType::Register) == 0x01);
    assert(static_cast<uint8_t>(RegistrarMessageType::Heartbeat) == 0x02);
    assert(static_cast<uint8_t>(RegistrarMessageType::NodeJoin) == 0x03);
    assert(static_cast<uint8_t>(RegistrarMessageType::NodeLeave) == 0x04);
    assert(static_cast<uint8_t>(RegistrarMessageType::Accept) == 0x05);
    assert(static_cast<uint8_t>(RegistrarMessageType::Error) == 0x06);
    assert(static_cast<uint8_t>(RegistrarMessageType::ResolveQuery) == 0x10);
    assert(static_cast<uint8_t>(RegistrarMessageType::ResolveResponse) == 0x11);

    // Test NodeRegistry
    RegistrarConfig reg_config;
    NodeRegistry registry(reg_config);
    assert(registry.all().empty());

    NodeEndpoint ep2;
    ep2.node_id = 42;
    ep2.host = "192.168.1.100";
    ep2.tcp_port = 9001;
    ep2.is_static_route = true;
    registry.upsert_endpoint(ep2);

    assert(registry.has(42));
    assert(registry.get(42)->tcp_port == 9001);
    assert(registry.all().size() == 1);

    std::cout << "All registrar tests passed" << std::endl;
    return 0;
}