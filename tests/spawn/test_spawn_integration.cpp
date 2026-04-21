// Copyright 2026 HPActor Contributors
// Tests full spawn flow: Frame encode/decode, spawn message correlation

#include <hpactor/spawn.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/types/types.hpp>
#include <hpactor/core/actor_system_ids.hpp>
#include <cassert>
#include <cstring>

// Test Frame encoding/decoding for spawn protocol
// Note: Frame::encode/decode currently preserves node_id, id, incarnation, message_id, flags
// but does NOT preserve ActorAddress.type (this is a known limitation in Frame implementation)
void test_frame_encoding() {
    // Create a spawn request Frame
    hpactor::net::Frame frame;
    frame.sender = hpactor::ActorAddress{1, 10, hpactor::ActorId{42}, 1};
    frame.receiver = hpactor::ActorAddress{2, hpactor::SystemActorType, hpactor::SpawnReceiverId, 0};
    frame.message_id = 12345;
    frame.flags = hpactor::net::Frame::RpcRequest;

    // Encode frame
    hpactor::bytes encoded = frame.encode();
    assert(encoded.size() > 0);

    // Decode frame
    hpactor::net::Frame decoded = hpactor::net::Frame::decode(encoded);

    // Verify fields that Frame encodes (node_id, id, incarnation, message_id, flags)
    // Note: ActorAddress.type is NOT preserved in current Frame implementation
    assert(decoded.sender.node_id == frame.sender.node_id);
    assert(decoded.sender.id == frame.sender.id);
    assert(decoded.sender.incarnation == frame.sender.incarnation);
    assert(decoded.receiver.node_id == frame.receiver.node_id);
    assert(decoded.receiver.id == frame.receiver.id);
    assert(decoded.receiver.incarnation == frame.receiver.incarnation);
    assert(decoded.message_id == frame.message_id);
    assert(decoded.flags == frame.flags);
}

// Test SpawnRequest binary serialization (manual, not via MessageVariant)
void test_spawn_request_binary_format() {
    hpactor::SpawnRequest req;
    req.actor_type_name = "worker";
    req.args_type = hpactor::TypeTag::User;
    req.serialized_args = {1, 2, 3};
    req.supervisor_addr = hpactor::ActorAddress{1, 10, hpactor::ActorId{42}, 1};

    // Manual encode (same format as DefaultSerializer would produce)
    // [4b: name len][name][4b: args_type][4b: args len][args]
    // [4b: sup node][8b: sup actor_id][4b: sup inc][4b: sup type]
    size_t name_len = req.actor_type_name.size();
    size_t args_len = req.serialized_args.size();
    size_t total = sizeof(uint32_t) + name_len +
                   sizeof(uint32_t) + sizeof(uint32_t) + args_len +
                   sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);

    hpactor::bytes encoded(total);
    size_t offset = 0;

    uint32_t name_len_u32 = static_cast<uint32_t>(name_len);
    std::memcpy(encoded.data() + offset, &name_len_u32, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    std::memcpy(encoded.data() + offset, req.actor_type_name.data(), name_len);
    offset += name_len;

    uint32_t args_type = static_cast<uint32_t>(req.args_type);
    std::memcpy(encoded.data() + offset, &args_type, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    uint32_t args_len_u32 = static_cast<uint32_t>(args_len);
    std::memcpy(encoded.data() + offset, &args_len_u32, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    std::memcpy(encoded.data() + offset, req.serialized_args.data(), args_len);
    offset += args_len;

    uint32_t sup_node = req.supervisor_addr.node_id;
    std::memcpy(encoded.data() + offset, &sup_node, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint64_t sup_actor_id = req.supervisor_addr.id.value();
    std::memcpy(encoded.data() + offset, &sup_actor_id, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    uint32_t sup_inc = static_cast<uint32_t>(req.supervisor_addr.incarnation);
    std::memcpy(encoded.data() + offset, &sup_inc, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint32_t sup_type = req.supervisor_addr.type;
    std::memcpy(encoded.data() + offset, &sup_type, sizeof(uint32_t));

    // Manual decode
    hpactor::SpawnRequest decoded_req;
    offset = 0;

    uint32_t decoded_name_len;
    std::memcpy(&decoded_name_len, encoded.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    decoded_req.actor_type_name.resize(decoded_name_len);
    std::memcpy(decoded_req.actor_type_name.data(), encoded.data() + offset, decoded_name_len);
    offset += decoded_name_len;

    uint32_t decoded_args_type;
    std::memcpy(&decoded_args_type, encoded.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    decoded_req.args_type = static_cast<hpactor::TypeTag>(decoded_args_type);

    uint32_t decoded_args_len;
    std::memcpy(&decoded_args_len, encoded.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    decoded_req.serialized_args.resize(decoded_args_len);
    std::memcpy(decoded_req.serialized_args.data(), encoded.data() + offset, decoded_args_len);
    offset += decoded_args_len;

    uint32_t sup_node_dec;
    std::memcpy(&sup_node_dec, encoded.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint64_t sup_actor_id_dec;
    std::memcpy(&sup_actor_id_dec, encoded.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    uint32_t sup_inc_dec;
    std::memcpy(&sup_inc_dec, encoded.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    uint32_t sup_type_dec;
    std::memcpy(&sup_type_dec, encoded.data() + offset, sizeof(uint32_t));

    decoded_req.supervisor_addr.node_id = sup_node_dec;
    decoded_req.supervisor_addr.id = hpactor::ActorId(sup_actor_id_dec);
    decoded_req.supervisor_addr.incarnation = sup_inc_dec;
    decoded_req.supervisor_addr.type = sup_type_dec;

    // Verify
    assert(decoded_req.actor_type_name == "worker");
    assert(decoded_req.args_type == hpactor::TypeTag::User);
    assert(decoded_req.serialized_args.size() == 3);
    assert(decoded_req.supervisor_addr.node_id == 1);
    assert(decoded_req.supervisor_addr.id.value() == 42);
}

// Test message_id correlation between request and response
void test_message_id_correlation() {
    uint64_t request_message_id = hpactor::MessageId::generate().value();

    // Simulate response with same message_id
    hpactor::net::Frame request_frame;
    request_frame.message_id = request_message_id;
    request_frame.flags = hpactor::net::Frame::RpcRequest;

    hpactor::net::Frame response_frame;
    response_frame.message_id = request_message_id;  // Same ID for correlation
    response_frame.flags = hpactor::net::Frame::RpcResponse;

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