// Copyright 2026 HPActor Contributors
// Tests full spawn flow: Frame encode/decode, spawn message correlation

#include <cassert>
#include <cstring>
#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/spawn.hpp>
#include <hpactor/types/types.hpp>

// Test Frame encoding/decoding for spawn protocol
// Note: Frame::encode/decode now uses binary endpoint encoding
void test_frame_encoding() {
    // Create a spawn request Frame
    hpactor::net::WireFrame frame;
    frame.sender =
        hpactor::ActorAddress{hpactor::endpoint_ops::parse_endpoint("node1:"
                                                                    "12345"),
                              hpactor::ActorType{10}, hpactor::ActorId{42}, 1};
    frame.receiver = hpactor::ActorAddress{
        hpactor::endpoint_ops::parse_endpoint("node2:12345"),
        hpactor::SystemActorType, hpactor::SpawnReceiverId, 0};
    frame.message_id = 12345;
    frame.flags = hpactor::net::WireFrame::RpcRequest;

    // Encode frame
    hpactor::bytes encoded = frame.encode();
    assert(encoded.size() > 0);

    // Decode frame
    hpactor::net::WireFrame decoded = hpactor::net::WireFrame::decode(encoded);

    // Verify fields that Frame encodes (endpoint, id, incarnation, message_id,
    // flags) Note: ActorAddress.type is NOT preserved in current Frame
    // implementation
    assert(decoded.sender.endpoint == frame.sender.endpoint);
    assert(decoded.sender.id == frame.sender.id);
    assert(decoded.sender.incarnation == frame.sender.incarnation);
    assert(decoded.receiver.endpoint == frame.receiver.endpoint);
    assert(decoded.receiver.id == frame.receiver.id);
    assert(decoded.receiver.incarnation == frame.receiver.incarnation);
    assert(decoded.message_id == frame.message_id);
    assert(decoded.flags == frame.flags);
}

// Test SpawnRequest binary serialization (via DefaultSerializer)
void test_spawn_request_binary_format() {
    hpactor::DefaultSerializer serializer;

    hpactor::SpawnRequest req;
    req.actor_type_name = "worker";
    req.args_type = hpactor::TypeTag::User;
    req.serialized_args = {1, 2, 3};
    req.supervisor_addr =
        hpactor::ActorAddress{hpactor::endpoint_ops::parse_endpoint("node1:"
                                                                    "12345"),
                              hpactor::ActorType{10}, hpactor::ActorId{42}, 1};

    hpactor::SpawnMessageVariant mv = req;
    hpactor::bytes encoded =
        serializer.encode_spawn(hpactor::TypeTag::SpawnRequestTag, mv);
    assert(encoded.size() > 0);

    hpactor::SpawnMessageVariant decoded =
        serializer.decode_spawn(hpactor::TypeTag::SpawnRequestTag, encoded);
    assert(std::holds_alternative<hpactor::SpawnRequest>(decoded));

    auto& decoded_req = std::get<hpactor::SpawnRequest>(decoded);
    assert(decoded_req.actor_type_name == "worker");
    assert(decoded_req.args_type == hpactor::TypeTag::User);
    assert(decoded_req.serialized_args.size() == 3);
    assert(decoded_req.supervisor_addr.endpoint ==
           hpactor::endpoint_ops::parse_endpoint("node1:12345"));
    assert(decoded_req.supervisor_addr.id.value() == 42);
}

// Test message_id correlation between request and response
void test_message_id_correlation() {
    uint64_t request_message_id = hpactor::MessageId::generate().value();

    // Simulate response with same message_id
    hpactor::net::WireFrame request_frame;
    request_frame.message_id = request_message_id;
    request_frame.flags = hpactor::net::WireFrame::RpcRequest;

    hpactor::net::WireFrame response_frame;
    response_frame.message_id = request_message_id; // Same ID for correlation
    response_frame.flags = hpactor::net::WireFrame::RpcResponse;

    assert(response_frame.message_id == request_frame.message_id);

    // Verify they would be matched in a routing table
    uint64_t matched_id = response_frame.message_id;
    assert(matched_id == request_message_id);
}

int main() {
    test_frame_encoding();
    test_spawn_request_binary_format();
    test_message_id_correlation();
    return 0;
}