// Copyright 2026 HPActor Contributors
#include <hpactor/net/frame.hpp>

// Include the generated protobuf headers
#include <hpactor/common.pb.h>
#include <hpactor/frame.pb.h>

namespace hpactor {
namespace net {
namespace protobuf {

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

// Helper: convert HPActor CommunicationEndpoint to protobuf PbActorEndpoint
static void to_proto(::hpactor::PbActorEndpoint* pb_endpoint,
                     const ::hpactor::CommunicationEndpoint& ep) {
    if (auto* ipv4 = std::get_if<::hpactor::Ipv4Endpoint>(&ep)) {
        to_proto(pb_endpoint->mutable_ipv4(), *ipv4);
    } else if (auto* ipv6 = std::get_if<::hpactor::Ipv6Endpoint>(&ep)) {
        to_proto(pb_endpoint->mutable_ipv6(), *ipv6);
    }
}

// Helper: convert HPActor ActorAddress to protobuf PbActorAddress
static void
to_proto(::hpactor::PbActorAddress* pb_addr, const ::hpactor::ActorAddress& addr) {
    to_proto(pb_addr->mutable_endpoint(), addr.endpoint);
    pb_addr->set_type(addr.type);
    pb_addr->set_actor_id(addr.id.value());
    pb_addr->set_incarnation(addr.incarnation);
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

// Helper: convert protobuf PbActorEndpoint to HPActor CommunicationEndpoint
static ::hpactor::CommunicationEndpoint
from_proto(const ::hpactor::PbActorEndpoint& pb_endpoint) {
    if (pb_endpoint.has_ipv4()) {
        return from_proto(pb_endpoint.ipv4());
    } else if (pb_endpoint.has_ipv6()) {
        return from_proto(pb_endpoint.ipv6());
    }
    return ::hpactor::Ipv4Endpoint{};
}

// Helper: convert protobuf PbActorAddress to HPActor ActorAddress
static ::hpactor::ActorAddress
from_proto(const ::hpactor::PbActorAddress& pb_addr) {
    return ::hpactor::ActorAddress{
        from_proto(pb_addr.endpoint()),
        static_cast<::hpactor::ActorType>(pb_addr.type()),
        ::hpactor::ActorId{pb_addr.actor_id()}, pb_addr.incarnation()};
}

} // namespace protobuf

bytes frame_to_proto(const WireFrame& frame) {
    ::hpactor::net::Frame pb_frame;
    protobuf::to_proto(pb_frame.mutable_sender(), frame.sender);
    protobuf::to_proto(pb_frame.mutable_receiver(), frame.receiver);
    pb_frame.set_message_id(frame.message_id);
    pb_frame.set_flags(frame.flags);
    pb_frame.set_payload(frame.payload.data(), frame.payload.size());
    pb_frame.set_type_tag(frame.type_tag);

    std::string serialized = pb_frame.SerializeAsString();
    return bytes(serialized.begin(), serialized.end());
}

WireFrame frame_from_proto(const bytes& data) {
    ::hpactor::net::Frame pb_frame;
    std::string serialized(data.begin(), data.end());
    if (!pb_frame.ParseFromString(serialized)) {
        return WireFrame{}; // Return default frame on parse failure
    }

    WireFrame frame;
    frame.sender = protobuf::from_proto(pb_frame.sender());
    frame.receiver = protobuf::from_proto(pb_frame.receiver());
    frame.message_id = pb_frame.message_id();
    frame.flags = pb_frame.flags();
    frame.payload.assign(pb_frame.payload().begin(), pb_frame.payload().end());
    frame.type_tag = pb_frame.type_tag();
    return frame;
}

} // namespace net
} // namespace hpactor
