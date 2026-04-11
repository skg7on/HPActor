#include <hpactor/net/frame.hpp>

#include <cstring>

namespace hpactor {

namespace net {

bytes Frame::encode() const {
    bytes result;
    result.resize(FrameHeaderSize + payload.size());

    uint32_t payload_len = static_cast<uint32_t>(payload.size());
    uint32_t type_tag = 0;  // TODO: extract type tag from payload
    uint64_t msg_id = message_id;

    size_t offset = 0;

    // Payload length
    std::memcpy(result.data() + offset, &payload_len, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Type tag
    std::memcpy(result.data() + offset, &type_tag, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Flags
    std::memcpy(result.data() + offset, &flags, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Message ID
    std::memcpy(result.data() + offset, &msg_id, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Sender node_id
    std::memcpy(result.data() + offset, &sender.node_id, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Sender actor_id
    uint64_t sender_id_val = sender.id.value();
    std::memcpy(result.data() + offset, &sender_id_val, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Sender incarnation
    std::memcpy(result.data() + offset, &sender.incarnation, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Receiver node_id
    std::memcpy(result.data() + offset, &receiver.node_id, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Receiver actor_id
    uint64_t receiver_id_val = receiver.id.value();
    std::memcpy(result.data() + offset, &receiver_id_val, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Receiver incarnation
    std::memcpy(result.data() + offset, &receiver.incarnation, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Payload
    if (!payload.empty()) {
        std::memcpy(result.data() + offset, payload.data(), payload.size());
    }

    return result;
}

Frame Frame::decode(const bytes& data) {
    Frame frame;
    size_t offset = 0;

    // Skip payload length (not needed for parsing)
    offset += sizeof(uint32_t);

    // Type tag (placeholder - would be used for dispatch)
    offset += sizeof(uint32_t);

    // Flags
    std::memcpy(&frame.flags, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Message ID
    std::memcpy(&frame.message_id, data.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Sender
    std::memcpy(&frame.sender.node_id, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    uint64_t sender_id_val;
    std::memcpy(&sender_id_val, data.data() + offset, sizeof(uint64_t));
    frame.sender.id = ActorId(sender_id_val);
    offset += sizeof(uint64_t);

    std::memcpy(&frame.sender.incarnation, data.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Receiver
    std::memcpy(&frame.receiver.node_id, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    uint64_t receiver_id_val;
    std::memcpy(&receiver_id_val, data.data() + offset, sizeof(uint64_t));
    frame.receiver.id = ActorId(receiver_id_val);
    offset += sizeof(uint64_t);

    std::memcpy(&frame.receiver.incarnation, data.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Payload - remaining data
    if (offset < data.size()) {
        auto it = data.begin() + static_cast<long>(offset);
        frame.payload.insert(frame.payload.end(), it, data.end());
    }

    return frame;
}

} // namespace net
} // namespace hpactor
