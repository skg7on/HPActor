// Copyright 2026 HPActor Contributors
// Tests full spawn flow: Frame encode/decode, spawn message correlation

#include <cassert>
#include <cstring>
#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/spawn.hpp>
#include <hpactor/types/types.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

// Test Frame encoding/decoding for spawn protocol
void test_frame_encoding() {
    hpactor::net::WireFrame frame;
    auto snd_addr =
        hpactor::ActorAddress{hpactor::endpoint_ops::parse_endpoint("node1:12345"),
                              hpactor::ActorType{10}, hpactor::ActorId{42}, 1};
    auto rcv_addr = hpactor::ActorAddress{
        hpactor::endpoint_ops::parse_endpoint("node2:12345"),
        hpactor::SystemActorType, hpactor::SpawnReceiverId, 0};

    hpactor::net::to_proto(frame.pb_frame.mutable_sender(), snd_addr);
    hpactor::net::to_proto(frame.pb_frame.mutable_receiver(), rcv_addr);
    frame.pb_frame.set_message_id(12345);
    frame.pb_frame.set_flags(hpactor::net::WireFrame::RpcRequest);

    hpactor::StreamBuffer encoded = frame.encode();
    assert(encoded.size() > 0);

    hpactor::net::WireFrame decoded = hpactor::net::WireFrame::decode(encoded);

    auto dec_sender = hpactor::net::from_proto(decoded.pb_frame.sender());
    auto dec_receiver = hpactor::net::from_proto(decoded.pb_frame.receiver());
    assert(dec_sender.endpoint == snd_addr.endpoint);
    assert(dec_sender.id == snd_addr.id);
    assert(dec_sender.incarnation == snd_addr.incarnation);
    assert(dec_receiver.endpoint == rcv_addr.endpoint);
    assert(dec_receiver.id == rcv_addr.id);
    assert(dec_receiver.incarnation == rcv_addr.incarnation);
    assert(decoded.pb_frame.message_id() == frame.pb_frame.message_id());
    assert(decoded.pb_frame.flags() == frame.pb_frame.flags());
}

// Test SpawnRequest protobuf serialization via ProtoTypeRegistry
void test_spawn_request_protobuf() {
    hpactor::ProtoTypeRegistry registry;
    registry.register_system_types();

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name("worker");
    pb_req.set_args_type(static_cast<uint32_t>(hpactor::TypeTag::User));
    pb_req.set_serialized_args("abc");
    auto* sup = pb_req.mutable_supervisor();
    sup->mutable_global_addr()->mutable_endpoint()->mutable_ipv4()->set_addr(0x7F000001);
    sup->mutable_global_addr()->mutable_endpoint()->mutable_ipv4()->set_port(8080);
    sup->mutable_global_addr()->mutable_local_addr()->set_actor_type(10);
    sup->mutable_global_addr()->mutable_local_addr()->set_actor_id(42);
    sup->mutable_global_addr()->mutable_local_addr()->set_incarnation(1);

    hpactor::StreamBuffer encoded = registry.serialize(pb_req);
    assert(encoded.size() > 0);

    auto decoded = registry.deserialize(hpactor::TypeTag::SpawnRequestTag, encoded);
    assert(decoded != nullptr);

    auto* decoded_req = static_cast<::hpactor::SpawnRequestMessage*>(decoded.get());
    assert(decoded_req->actor_type_name() == "worker");
    assert(decoded_req->args_type() == static_cast<uint32_t>(hpactor::TypeTag::User));
    assert(decoded_req->serialized_args() == "abc");
    assert(decoded_req->supervisor().global_addr().local_addr().actor_id() == 42);
}

// Test message_id correlation between request and response
void test_message_id_correlation() {
    uint64_t request_message_id = hpactor::MessageId::generate().value();

    hpactor::net::WireFrame request_frame;
    request_frame.pb_frame.set_message_id(request_message_id);
    request_frame.pb_frame.set_flags(hpactor::net::WireFrame::RpcRequest);

    hpactor::net::WireFrame response_frame;
    response_frame.pb_frame.set_message_id(request_message_id);
    response_frame.pb_frame.set_flags(hpactor::net::WireFrame::RpcResponse);

    assert(response_frame.pb_frame.message_id() == request_frame.pb_frame.message_id());

    uint64_t matched_id = response_frame.pb_frame.message_id();
    assert(matched_id == request_message_id);
}

int main() {
    test_frame_encoding();
    test_spawn_request_protobuf();
    test_message_id_correlation();
    return 0;
}
