// Copyright 2026 HPActor Contributors
#include <hpactor/net/frame.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/frame.pb.h>

namespace hpactor {
namespace net {

// Helper: convert HPActor Ipv4Endpoint to protobuf PbIpv4Endpoint
static void
to_proto(::hpactor::PbIpv4Endpoint* pb_ep, const ::hpactor::Ipv4Endpoint& ep) {
    pb_ep->set_addr(ep.addr); // Network byte order
    pb_ep->set_port(ep.port_nw);
}

// Helper: convert HPActor Ipv6Endpoint to protobuf PbIpv6Endpoint
static void
to_proto(::hpactor::PbIpv6Endpoint* pb_ep, const ::hpactor::Ipv6Endpoint& ep) {
    pb_ep->set_addr(ep.addr.data(), 16);
    pb_ep->set_port(ep.port_nw);
}

// Helper: convert HPActor EndPoint to protobuf PbActorEndpoint
static void to_proto(::hpactor::PbActorEndpoint* pb_endpoint,
                     const ::hpactor::EndPoint& ep) {
    if (auto* ipv4 = std::get_if<::hpactor::Ipv4Endpoint>(&ep)) {
        to_proto(pb_endpoint->mutable_ipv4(), *ipv4);
    } else if (auto* ipv6 = std::get_if<::hpactor::Ipv6Endpoint>(&ep)) {
        to_proto(pb_endpoint->mutable_ipv6(), *ipv6);
    }
}

// Helper: fill PbLocalActorAddress from C++ ActorAddress fields
static void fill_local_addr(::hpactor::PbLocalActorAddress* local,
                            const ActorAddress& addr) {
    local->set_actor_type(addr.type);
    local->set_actor_id(addr.id.value());
    local->set_incarnation(addr.incarnation);
}

// Helper: fill PbGlobalActorAddress from C++ ActorAddress fields
static void fill_global_addr(::hpactor::PbGlobalActorAddress* global,
                             const ActorAddress& addr) {
    to_proto(global->mutable_endpoint(), addr.endpoint);
    fill_local_addr(global->mutable_local_addr(), addr);
}

// Returns true if the endpoint is the default localhost:0 (no real endpoint).
static bool is_default_local_endpoint(const EndPoint& ep) {
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
        return ipv4->is_loopback() && ipv4->port() == 0;
    }
    if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&ep)) {
        return ipv6->is_loopback() && ipv6->port() == 0;
    }
    return false;
}

// Helper: convert C++ ActorAddress to protobuf PbActorAddress
// Uses local_addr only for the default localhost:0 endpoint;
// all other addresses (including IPv6 loopback) use global_addr
// to preserve full endpoint information.
void to_proto(::hpactor::PbActorAddress* pb_addr, const ActorAddress& addr) {
    if (is_default_local_endpoint(addr.endpoint) || !addr) {
        fill_local_addr(pb_addr->mutable_local_addr(), addr);
    } else {
        fill_global_addr(pb_addr->mutable_global_addr(), addr);
    }
}

// Helper: convert C++ ActorAddress to protobuf PbActorRef (same structure)
void to_proto(::hpactor::PbActorRef* pb_ref, const ActorAddress& addr) {
    if (is_default_local_endpoint(addr.endpoint) || !addr) {
        fill_local_addr(pb_ref->mutable_local_addr(), addr);
    } else {
        fill_global_addr(pb_ref->mutable_global_addr(), addr);
    }
}

// Helper: convert protobuf PbIpv4Endpoint to HPActor Ipv4Endpoint
static ::hpactor::Ipv4Endpoint from_proto(const ::hpactor::PbIpv4Endpoint& pb_ep) {
    return ::hpactor::Ipv4Endpoint{pb_ep.addr(),
                                   static_cast<uint16_t>(pb_ep.port())};
}

// Helper: convert protobuf PbIpv6Endpoint to HPActor Ipv6Endpoint
static ::hpactor::Ipv6Endpoint from_proto(const ::hpactor::PbIpv6Endpoint& pb_ep) {
    std::array<uint8_t, 16> addr;
    std::memcpy(addr.data(), pb_ep.addr().data(), 16);
    return ::hpactor::Ipv6Endpoint{addr, static_cast<uint16_t>(pb_ep.port())};
}

// Helper: convert protobuf PbActorEndpoint to HPActor EndPoint
static ::hpactor::EndPoint
from_proto(const ::hpactor::PbActorEndpoint& pb_endpoint) {
    if (pb_endpoint.has_ipv4()) {
        return from_proto(pb_endpoint.ipv4());
    } else if (pb_endpoint.has_ipv6()) {
        return from_proto(pb_endpoint.ipv6());
    }
    return ::hpactor::Ipv4Endpoint{};
}

// Helper: build ActorAddress from PbLocalActorAddress fields
static ActorAddress from_local_addr(const ::hpactor::PbLocalActorAddress& local) {
    return ActorAddress{
        Ipv4Endpoint{0x7F000001, 0},
        static_cast<ActorType>(local.actor_type()),
        ActorId{local.actor_id()},
        local.incarnation()};
}

// Helper: build ActorAddress from PbGlobalActorAddress fields
static ActorAddress from_global_addr(const ::hpactor::PbGlobalActorAddress& global) {
    const auto& local = global.local_addr();
    return ActorAddress{
        from_proto(global.endpoint()),
        static_cast<ActorType>(local.actor_type()),
        ActorId{local.actor_id()},
        local.incarnation()};
}

// Helper: convert protobuf PbActorAddress to HPActor ActorAddress
ActorAddress from_proto(const ::hpactor::PbActorAddress& pb_addr) {
    if (pb_addr.has_local_addr()) {
        return from_local_addr(pb_addr.local_addr());
    }
    if (pb_addr.has_global_addr()) {
        return from_global_addr(pb_addr.global_addr());
    }
    return ActorAddress{};
}

// Helper: convert protobuf PbActorRef to HPActor ActorAddress
ActorAddress from_proto(const ::hpactor::PbActorRef& pb_ref) {
    if (pb_ref.has_local_addr()) {
        return from_local_addr(pb_ref.local_addr());
    }
    if (pb_ref.has_global_addr()) {
        return from_global_addr(pb_ref.global_addr());
    }
    return ActorAddress{};
}

} // namespace net
} // namespace hpactor
