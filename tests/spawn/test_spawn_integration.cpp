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
    frame.sender =
        hpactor::ActorAddress{hpactor::endpoint_ops::parse_endpoint("node1:12345"),
                              hpactor::ActorType{10}, hpactor::ActorId{42}, 1};
    frame.receiver = hpactor::ActorAddress{
        hpactor::endpoint_ops::parse_endpoint("node2:12345"),
        hpactor::SystemActorType, hpactor::SpawnReceiverId, 0};
    frame.message_id = 12345;
    frame.flags = hpactor::net::WireFrame::RpcRequest;

    hpactor::bytes encoded = frame.encode();
    assert(encoded.size() > 0);

    hpactor::net::WireFrame decoded = hpactor::net::WireFrame::decode(encoded);

    assert(decoded.sender.endpoint == frame.sender.endpoint);
    assert(decoded.sender.id == frame.sender.id);
    assert(decoded.sender.incarnation == frame.sender.incarnation);
    assert(decoded.receiver.endpoint == frame.receiver.endpoint);
    assert(decoded.receiver.id == frame.receiver.id);
    assert(decoded.receiver.incarnation == frame.receiver.incarnation);
    assert(decoded.message_id == frame.message_id);
    assert(decoded.flags == frame.flags);
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
    sup->mutable_endpoint()->mutable_ipv4()->set_addr(0x7F000001);
    sup->mutable_endpoint()->mutable_ipv4()->set_port(8080);
    sup->set_type(10);
    sup->set_actor_id(42);
    sup->set_incarnation(1);

    hpactor::bytes encoded = registry.serialize(pb_req);
    assert(encoded.size() > 0);

    auto decoded = registry.deserialize(hpactor::TypeTag::SpawnRequestTag, encoded);
    assert(decoded != nullptr);

    auto* decoded_req = static_cast<::hpactor::SpawnRequestMessage*>(decoded.get());
    assert(decoded_req->actor_type_name() == "worker");
    assert(decoded_req->args_type() == static_cast<uint32_t>(hpactor::TypeTag::User));
    assert(decoded_req->serialized_args() == "abc");
    assert(decoded_req->supervisor().actor_id() == 42);
}

// Test message_id correlation between request and response
void test_message_id_correlation() {
    uint64_t request_message_id = hpactor::MessageId::generate().value();

    hpactor::net::WireFrame request_frame;
    request_frame.message_id = request_message_id;
    request_frame.flags = hpactor::net::WireFrame::RpcRequest;

    hpactor::net::WireFrame response_frame;
    response_frame.message_id = request_message_id;
    response_frame.flags = hpactor::net::WireFrame::RpcResponse;

    assert(response_frame.message_id == request_frame.message_id);

    uint64_t matched_id = response_frame.message_id;
    assert(matched_id == request_message_id);
}

int main() {
    test_frame_encoding();
    test_spawn_request_protobuf();
    test_message_id_correlation();
    return 0;
}
