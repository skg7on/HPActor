#include <hpactor/net/registrar.hpp>

#include <cassert>
#include <iostream>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test RegistrarConfig defaults
    RegistrarConfig config;
    assert(config.udp_port == 5353);
    assert(config.heartbeat_interval.count() == 5000);
    assert(config.expiration_timeout.count() == 15000);
    assert(config.static_routes.empty());
    assert(config.enable_broadcast == true);

    // Test StaticRouteConfig defaults
    StaticRouteConfig route;
    assert(route.node_id == 0);
    assert(route.port == 0);

    // Test NodeEndpoint defaults
    NodeEndpoint ep;
    assert(ep.node_id == 0);
    assert(ep.tcp_port == 0);
    assert(ep.is_static_route == false);

    // Test HostResolver with IP address (no DNS needed)
    HostResolver resolver;
    resolver.cache("192.168.1.1", "192.168.1.1", std::chrono::seconds(60));
    assert(resolver.get_cached("192.168.1.1") == "192.168.1.1");
    assert(resolver.get_cached("unknown") == "");

    // Test RegistrarMessageType enum values
    assert(static_cast<uint8_t>(RegistrarMessageType::NodeAnnounce) == 0x01);
    assert(static_cast<uint8_t>(RegistrarMessageType::NodeQuery) == 0x02);
    assert(static_cast<uint8_t>(RegistrarMessageType::NodeResponse) == 0x03);
    assert(static_cast<uint8_t>(RegistrarMessageType::NodeLeave) == 0x04);
    assert(static_cast<uint8_t>(RegistrarMessageType::NodeProbe) == 0x05);
    assert(static_cast<uint8_t>(RegistrarMessageType::NodeProbeAck) == 0x06);

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